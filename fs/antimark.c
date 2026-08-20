/* SPDX-License-Identifier: GPL-2.0 */
/*
 * antimark.c - AntiMark 内核反指纹引擎
 * 值全部由内核内存托管：sysfs 假值表、MAC、serial。
 * userspace 只能通过 reboot() 魔数通道 + 令牌访问，root 进程同样读不到真值。
 */
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/kernfs.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/random.h>
#include <linux/antimark.h>
#include <linux/shmem_fs.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/vmalloc.h>

/* ==================== 令牌 ==================== */
static u8 am_token[AM_TOKEN_LEN] = AM_DEFAULT_TOKEN;
static DEFINE_MUTEX(am_lock);

static bool am_check_token(__u32 tok)
{
	return memcmp(&tok, am_token, AM_TOKEN_LEN) == 0;
}

/* ==================== sysfs 假值表 ==================== */
struct am_fake_entry {
	char path[AM_MAX_PATH];
	char value[AM_MAX_VALUE];
	bool used;
};

static struct am_fake_entry am_fake_table[AM_MAX_FAKE_ENTRIES];

/*
 * 由 fs/sysfs/file.c 的 sysfs_kf_seq_show() 调用。
 * of->kn 的 kernfs_path() 是相对 /sys 的路径，如 "devices/soc0/serial_number"。
 * 命中：输出假值返回 0；未命中返回 -EAGAIN 走原流程。
 */
int antimark_sysfs_seq_show(struct kernfs_open_file *of, char *buf, size_t size)
{
	char path[AM_MAX_PATH];
	int i;
	int len;

	len = kernfs_path(of->kn, path, sizeof(path));
	if (len <= 0)
		return -EAGAIN;

	mutex_lock(&am_lock);
	for (i = 0; i < AM_MAX_FAKE_ENTRIES; i++) {
		if (am_fake_table[i].used &&
		    strcmp(am_fake_table[i].path, path) == 0) {
			len = snprintf(buf, size, "%s", am_fake_table[i].value);
			mutex_unlock(&am_lock);
			return len;
		}
	}
	mutex_unlock(&am_lock);
	return -EAGAIN;
}

static int am_sysfs_fake_add(const char *data, u32 len)
{
	const char *sep;
	int path_len, value_len;
	int slot = -1;
	int i;

	sep = memchr(data, 0, len);
	if (!sep)
		return -EINVAL;
	path_len = sep - data;
	value_len = len - path_len - 1;
	if (path_len <= 0 || path_len >= AM_MAX_PATH ||
	    value_len <= 0 || value_len >= AM_MAX_VALUE)
		return -EINVAL;

	mutex_lock(&am_lock);
	/* 同名路径先覆盖 */
	for (i = 0; i < AM_MAX_FAKE_ENTRIES; i++) {
		if (am_fake_table[i].used &&
		    strcmp(am_fake_table[i].path, data) == 0) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		for (i = 0; i < AM_MAX_FAKE_ENTRIES; i++) {
			if (!am_fake_table[i].used) {
				slot = i;
				break;
			}
		}
	}
	if (slot < 0) {
		mutex_unlock(&am_lock);
		return -ENOSPC;
	}
	memcpy(am_fake_table[slot].path, data, path_len);
	am_fake_table[slot].path[path_len] = 0;
	memcpy(am_fake_table[slot].value, sep + 1, value_len);
	am_fake_table[slot].value[value_len] = 0;
	am_fake_table[slot].used = true;
	mutex_unlock(&am_lock);
	return 0;
}

static int am_sysfs_fake_del(const char *data)
{
	int i;

	mutex_lock(&am_lock);
	for (i = 0; i < AM_MAX_FAKE_ENTRIES; i++) {
		if (am_fake_table[i].used &&
		    strcmp(am_fake_table[i].path, data) == 0) {
			am_fake_table[i].used = false;
			mutex_unlock(&am_lock);
			return 0;
		}
	}
	mutex_unlock(&am_lock);
	return -ENOENT;
}

static void am_sysfs_fake_clear(void)
{
	int i;

	mutex_lock(&am_lock);
	for (i = 0; i < AM_MAX_FAKE_ENTRIES; i++)
		am_fake_table[i].used = false;
	mutex_unlock(&am_lock);
}

/* ==================== MAC ==================== */
static int am_set_mac(const char *data, u32 len)
{
	struct net_device *nd;
	char ifname[IFNAMSIZ];
	const u8 *mac;
	int name_len;

	if (len < IFNAMSIZ || len < 7)
		return -EINVAL;
	name_len = strnlen(data, IFNAMSIZ - 1);
	if (name_len == 0)
		return -EINVAL;
	memcpy(ifname, data, name_len);
	ifname[name_len] = 0;
	mac = (const u8 *)(data + name_len + 1);

	nd = dev_get_by_name(&init_net, ifname);
	if (!nd)
		return -ENODEV;
	dev_addr_set(nd, mac);
	dev_put(nd);
	return 0;
}

/* ==================== 文件伪装（内核态假库，无挂载痕迹） ==================== */
struct am_fake_file {
	char path[AM_MAX_PATH];
	struct dentry *dentry;
	struct file *shmem;
	bool used;
	bool active;
};
static struct am_fake_file am_fake_files[AM_MAX_FAKE_FILES];
static bool am_fake_enabled;
static void *am_fake_buf;
static size_t am_fake_buf_len;
static size_t am_fake_buf_cap;
static char am_fake_pending[AM_MAX_PATH];

/*
 * 由 fs/open.c vfs_open() 调用。dentry 命中则把 filp 掉包为 shmem 假文件：
 * mmap/read 全部读到假数据，但 f_path 保持原路径，/proc/mounts 无任何痕迹。
 * 返回 1 = 已替换（跳过 do_dentry_open）。
 */
int antimark_fake_open_check(const struct path *path, struct file *filp)
{
	int i;

	if (!am_fake_enabled)
		return 0;
	for (i = 0; i < AM_MAX_FAKE_FILES; i++) {
		struct am_fake_file *f = &am_fake_files[i];
		if (!f->used || !f->active || !f->dentry || !f->shmem)
			continue;
		if (path->dentry == f->dentry) {
			struct file *sf = f->shmem;
			struct inode *inode = sf->f_inode;

			get_file(sf);
			/* 补齐 do_dentry_open 的标准初始化（跳过它会 panic）：
			 * 1. path_get: __fput 会对 f_path dput/mntput，调用方
			 *    do_open() 也会 path_put，不拿独立引用 = double put
			 * 2. f_mode 补 FMODE_OPENED/CAN_READ/LSEEK/PREAD/PWRITE/ATOMIC_POS
			 * 3. f_ra/f_wb_err/f_iocb_flags 初始化（mmap/readahead 依赖） */
			path_get(&filp->f_path);
			filp->f_op = fops_get(sf->f_op);
			filp->f_mapping = inode->i_mapping;
			filp->f_inode = inode;
			ihold(inode);
			filp->f_wb_err = filemap_sample_wb_err(filp->f_mapping);
			filp->f_sb_err = file_sample_sb_err(filp);
			filp->f_mode = OPEN_FMODE(filp->f_flags) | FMODE_OPENED |
				       FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE |
				       FMODE_CAN_READ | FMODE_ATOMIC_POS;
			filp->f_iocb_flags = iocb_flags(filp);
			file_ra_state_init(&filp->f_ra,
					    filp->f_mapping->host->i_mapping);
			filp->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);
			filp->private_data = NULL;
			/*
			 * 对称：__fput() -> put_file_access() -> i_readcount_dec()
			 * 有 BUG_ON(dec < 0)，do_dentry_open 里 inc 过才能配对，
			 * 否则掉包 fd 关闭时 0 -> -1 直接 panic 重启。
			 */
			if ((filp->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ)
				i_readcount_inc(inode);
			return 1;
		}
	}
	return 0;
}

static void am_fake_buf_reset(void)
{
	vfree(am_fake_buf);
	am_fake_buf = NULL;
	am_fake_buf_len = 0;
	am_fake_buf_cap = 0;
}

static int am_fake_buf_write(const u8 *data, u32 len)
{
	u32 off = am_fake_buf_len;

	if (len == 0)
		return -EINVAL;
	if (off + len > am_fake_buf_cap) {
		size_t ncap = max(am_fake_buf_cap * 2, (size_t)(off + len + 4096));
		void *nbuf = vmalloc(ncap);
		if (!nbuf)
			return -ENOMEM;
		if (am_fake_buf)
			memcpy(nbuf, am_fake_buf, am_fake_buf_len);
		vfree(am_fake_buf);
		am_fake_buf = nbuf;
		am_fake_buf_cap = ncap;
	}
	memcpy((u8 *)am_fake_buf + off, data, len);
	am_fake_buf_len = off + len;
	return 0;
}

static int am_fake_commit(u32 total)
{
	struct file *sf;
	struct path p;
	struct am_fake_file *slot = NULL;
	loff_t pos = 0;
	int i, rc;

	if (kern_path(am_fake_pending, 0, &p))
		return -ENOENT;
	for (i = 0; i < AM_MAX_FAKE_FILES; i++) {
		if (!am_fake_files[i].used) { slot = &am_fake_files[i]; break; }
	}
	if (!slot) { rc = -ENOSPC; goto out_path; }

	if (total == 0) {
		/* 实体文件模式：假库落盘 /data/adb/antimark/fake/<basename>，
		 * 内核直接打开实体文件（无 shmem 内存拷贝，无 /proc 痕迹） */
		char fake_path[AM_MAX_PATH];
		const char *base;
		size_t blen;

		base = strrchr(am_fake_pending, '/');
		base = base ? base + 1 : am_fake_pending;
		blen = strlen(base);
		if (blen == 0 || 1 + 1 + blen + 1 > sizeof(fake_path)) {
			rc = -ENAMETOOLONG;
			goto out_path;
		}
		memcpy(fake_path, "/data/adb/antimark/fake/", sizeof("/data/adb/antimark/fake/"));
		memcpy(fake_path + sizeof("/data/adb/antimark/fake/") - 1, base, blen + 1);
		sf = filp_open(fake_path, O_RDONLY, 0);
		if (IS_ERR(sf)) { rc = PTR_ERR(sf); goto out_path; }
	} else {
		if (total > (16U << 20)) { rc = -EINVAL; goto out_path; }
		if (am_fake_buf_len < total) { rc = -EINVAL; goto out_path; }
		sf = shmem_file_setup("antimark_fake", total, 0);
		if (IS_ERR(sf)) { rc = PTR_ERR(sf); goto out_path; }
		if (kernel_write(sf, am_fake_buf, total, &pos) != total) {
			rc = -EIO;
			goto out_file;
		}
	}
	memset(slot, 0, sizeof(*slot));
	strscpy(slot->path, am_fake_pending, sizeof(slot->path));
	slot->dentry = dget(p.dentry);
	slot->shmem = sf;
	slot->used = true;
	slot->active = true;
	am_fake_enabled = true;
	am_fake_buf_reset();
	path_put(&p);
	return 0;
out_file:
	fput(sf);
out_path:
	path_put(&p);
	return rc;
}

static int am_fake_del(const char *path)
{
	int i;
	for (i = 0; i < AM_MAX_FAKE_FILES; i++) {
		struct am_fake_file *f = &am_fake_files[i];
		if (f->used && strcmp(f->path, path) == 0) {
			f->active = false;	/* 不再提供新 open；shmem 保留防悬空 */
			return 0;
		}
	}
	return -ENOENT;
}

static void am_fake_clear(void)
{
	int i;
	for (i = 0; i < AM_MAX_FAKE_FILES; i++)
		am_fake_files[i].active = false;
	am_fake_enabled = false;
	am_fake_buf_reset();
}

/* ==================== 命令分发 ==================== */
void antimark_handle_cmd(void __user **user_info)
{
	struct st_antimark_cmd info = {0};
	int rc = -EINVAL;

	if (copy_from_user(&info, *user_info, sizeof(info))) {
		rc = -EFAULT;
		goto out;
	}
	if (!am_check_token(info.token)) {
		rc = -EPERM;
		goto out;
	}

	switch (info.op) {
	case AM_OP_SET_TOKEN:
		if (info.len == AM_TOKEN_LEN) {
			mutex_lock(&am_lock);
			memcpy(am_token, info.data, AM_TOKEN_LEN);
			mutex_unlock(&am_lock);
			rc = 0;
		}
		break;
	case AM_OP_SET_SERIAL:
		/* 写入 sysfs 假值表：/sys/devices/soc0/serial_number */
		{
			char buf[AM_MAX_PATH + AM_MAX_VALUE];
			const char *p = "devices/soc0/serial_number";
			u32 plen = strlen(p);
			u32 vlen = info.len;

			if (vlen >= AM_MAX_VALUE)
				break;
			memcpy(buf, p, plen + 1);
			memcpy(buf + plen + 1, info.data, vlen);
			buf[plen + 1 + vlen] = 0;
			rc = am_sysfs_fake_add(buf, plen + 1 + vlen + 1);
		}
		break;
	case AM_OP_SET_MAC:
		rc = am_set_mac(info.data, info.len);
		break;
	case AM_OP_SYSFS_FAKE_ADD:
		rc = am_sysfs_fake_add(info.data, info.len);
		break;
	case AM_OP_SYSFS_FAKE_DEL:
		rc = am_sysfs_fake_del(info.data);
		break;
	case AM_OP_SYSFS_FAKE_CLEAR:
		am_sysfs_fake_clear();
		rc = 0;
		break;
	case AM_OP_SET_DRM_UID:
		/* 预留：drmid 精确 uid 走 open_redirect(UID_EXACT)，此处仅记录 */
		rc = 0;
		break;
	case AM_OP_FILE_FAKE_BEGIN: {
		size_t plen = strnlen(info.data, info.len);
		if (plen == 0 || plen >= AM_MAX_PATH)
			break;
		memcpy(am_fake_pending, info.data, plen + 1);
		am_fake_buf_reset();
		rc = 0;
		break;
	}
	case AM_OP_FILE_FAKE_WRITE:
		rc = am_fake_buf_write(info.data, info.len);
		break;
	case AM_OP_FILE_FAKE_COMMIT: {
		u32 total;
		if (info.len != 4)
			break;
		memcpy(&total, info.data, 4);
		rc = am_fake_commit(total);
		break;
	}
	case AM_OP_FILE_FAKE_DEL:
		rc = am_fake_del(info.data);
		break;
	case AM_OP_FILE_FAKE_CLEAR:
		am_fake_clear();
		rc = 0;
		break;
	default:
		break;
	}

out:
	info.err = rc;
	if (copy_to_user(&((struct st_antimark_cmd __user *)*user_info)->err,
			 &info.err, sizeof(info.err)))
		info.err = -EFAULT;
}
