/* SPDX-License-Identifier: GPL-2.0 */
/*
 * antimark.h - AntiMark 内核反指纹框架
 * 值全部由内核托管，userspace 只有经过令牌校验的接口才能读写
 */
#ifndef _LINUX_ANTIMARK_H
#define _LINUX_ANTIMARK_H

#include <linux/types.h>

#define ANTIMARK_MAGIC          0x414d4b53  /* "AMKS" */
#define CMD_ANTIMARK_SET        0x555d0

/* 操作码 */
#define AM_OP_SET_TOKEN         1   /* data: 8 字节新令牌 */
#define AM_OP_SET_SERIAL        2   /* data: 假 serial 字符串 (含\0) */
#define AM_OP_SET_MAC           3   /* data: ifname\0 + 6 字节 MAC */
#define AM_OP_SYSFS_FAKE_ADD    4   /* data: 路径\0 + 假值\0 */
#define AM_OP_SYSFS_FAKE_DEL    5   /* data: 路径\0 */
#define AM_OP_SYSFS_FAKE_CLEAR  6   /* 无 data */
#define AM_OP_SET_DRM_UID       7   /* data: 4 字节 uid (0xffffffff=清空) */
#define AM_OP_FILE_FAKE_BEGIN   8   /* data: 绝对路径\0  开始伪装该文件 */
#define AM_OP_FILE_FAKE_WRITE   9   /* data: offset(4 LE) + 数据块, 分块写入 */
#define AM_OP_FILE_FAKE_COMMIT  10  /* data: 总长度(4 LE), 完成并挂载假文件 */
#define AM_OP_FILE_FAKE_DEL     11  /* data: 绝对路径\0  停止伪装(不再新开) */
#define AM_OP_FILE_FAKE_CLEAR   12  /* 无 data: 停止全部伪装 */

#define AM_MAX_FAKE_ENTRIES     32
#define AM_MAX_FAKE_FILES       8
#define AM_MAX_PATH             128
#define AM_MAX_VALUE            128
#define AM_TOKEN_LEN            4
#define AM_MAX_DATA             512

/* 编译期默认令牌，boot 后模块可改 */
#define AM_DEFAULT_TOKEN        {0x0d, 0xf0, 0xfe, 0xba}

struct st_antimark_cmd {
	__u32 token;
	__u32 op;
	__u32 len;
	__u8  data[AM_MAX_DATA];
	int   err;
};

/* fs/sysfs/file.c 调用：命中返回 0 并已输出假值，未命中返回 -EAGAIN */
int antimark_sysfs_seq_show(struct kernfs_open_file *of, char *buf, size_t size);
/* kernel/reboot.c (KSU supercall) 调用 */
void antimark_handle_cmd(void __user **user_info);
/* fs/open.c vfs_open() 调用：文件伪装。命中返回 1 并已把 filp 掉包为假文件 */
struct path;
struct file;
int antimark_fake_open_check(const struct path *path, struct file *filp);

#endif /* _LINUX_ANTIMARK_H */
