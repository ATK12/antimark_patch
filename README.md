# antimark_patch

AntiMark 内核级反指纹补丁（AntiMark Kernel Patches）

内核侧实现，用户态配套（`antimark_ctl` / drmid 模块）不属于本仓库。

## 鸣谢

编译链路基于 [cctv18](https://github.com/cctv18) 的开源工作：

- [android_gki_kernel_common](https://github.com/cctv18/android_gki_kernel_common)：GKI OKI 内核源码
- [oneplus_sm8650_toolchain](https://github.com/cctv18/oneplus_sm8650_toolchain)：LLVM-Clang 工具链（clang/rust/build-tools）
- [susfs4oki](https://github.com/cctv18/susfs4oki)：SUSFS 补丁（0002 补丁依赖 `CONFIG_KSU_SUSFS`）
- [AnyKernel3](https://github.com/cctv18/AnyKernel3)：刷机包打包框架
- [Baseband-guard](https://github.com/cctv18/Baseband-guard)：内核级基带保护
- [KPatch-Next](https://github.com/cctv18/KPatch-Next)：KPM 支持
- public_ccache：公共 ccache 缓存
- oppo_oplus_realme_sm8850 / sm8750 / sm8650：编译 workflow 均 fork 自 cctv18 的仓库

## 原理

| 补丁 | 位置 | 作用 |
|------|------|------|
| 0001 | `fs/sysfs/file.c` + `fs/Makefile` | sysfs 假值表：`antimark_sysfs_seq_show()` 命中路径直接输出假值，不落盘 |
| 0002 | `KernelSU/kernel/supercall/dispatch.c` | KSU supercall 分发：`CMD_ANTIMARK_SET (0x555d0)` 路由到 `antimark_handle_cmd()` 注册掉包表 |
| 0003 | `fs/open.c` | `vfs_open()` 文件掉包：命中假表路径时把 `filp` 换成 shmem 假文件（无挂载，`/proc/mounts` 无痕迹） |

调用链：用户态 `antimark_ctl` → reboot supercall（magic1=`0xdeadbeef`, magic2=`0xfafafafa`, cmd=`0x555d0`）→ `ksu_handle_susfs_cmd()` → `antimark_handle_cmd()` 注册 → `vfs_open()` 命中掉包。

## 文件清单

```
antimark_patch/
├── 0001-antimark-sysfs.patch          # 通用（6.12 / 6.6 / 6.1）
├── 0002-antimark-dispatch.patch       # 通用（依赖 KSU + SUSFS）
├── 0003-antimark-vfs-fake.patch       # 仅 6.12（vfs_open 多行函数体）
├── 0003-antimark-vfs-fake-6.6-6.1.patch  # 仅 6.6/6.1（vfs_open 3 行函数体）
├── fs/antimark.c                      # 内核模块主实现（11364 B）
├── include/linux/antimark.h           # 头文件（CMD/OP 定义 + 接口声明）
└── anykernel.sh                       # 刷机包署名模板（改 ATK12 为你自己的）
```

0003 两版不可互换：6.12 的 `vfs_open` 是 `file->f_path = *path; ret = do_dentry_open(...)` 多行体，6.6/6.1 只有 3 行 `file->f_path = *path; return do_dentry_open(file, d_backing_inode(path->dentry), NULL);`。拿 6.12 版打 6.6/6.1 会被 `-F3` fuzz 插进 `dentry_create` 报 `undeclared identifier 'file'`。

## 前置条件

- 内核版本：6.12 / 6.6 / 6.1（GKI OKI）
- 集成 KernelSU / ReSukiSU（0002 改 `KernelSU/kernel/supercall/dispatch.c`）
- 开启 SUSFS（`CONFIG_KSU_SUSFS=y`）

`CONFIG_KSU_SUSFS` 必须为 `y`：`ksu_handle_susfs_cmd()` 整个函数体在 `#ifdef CONFIG_KSU_SUSFS` 内，未开启时被编译器删除，0002 补丁的 `CMD_ANTIMARK_SET` case 随之消失，supercall 返回 0 但不报错，掉包表注册不上（静默失效）。排查：`grep CONFIG_KSU_SUSFS arch/arm64/configs/gki_defconfig`。

## 编入步骤

假设内核源码根目录为 `$KERNEL`，补丁仓库 clone 到 `$PATCH`。

### 1. 拷贝源码

```sh
cp $PATCH/fs/antimark.c        $KERNEL/fs/antimark.c
cp $PATCH/include/linux/antimark.h  $KERNEL/include/linux/antimark.h
```

### 2. 应用补丁

```sh
cd $KERNEL

# 0001: sysfs 假值表（通用）
patch -p1 -F3 < $PATCH/0001-antimark-sysfs.patch

# 0002: KSU supercall 分发（通用；在 KernelSU 仓库目录下执行）
cd KernelSU
patch -p1 -F3 < $PATCH/0002-antimark-dispatch.patch
cd ..

# 0003: vfs_open 掉包（按内核版本选）
# 6.12:
patch -p1 -F3 < $PATCH/0003-antimark-vfs-fake.patch
# 6.6 / 6.1:
patch -p1 -F3 < $PATCH/0003-antimark-vfs-fake-6.6-6.1.patch
```

### 3. 开启 CONFIG

```sh
echo 'CONFIG_ANTIMARK=y' >> arch/arm64/configs/gki_defconfig
grep CONFIG_KSU_SUSFS arch/arm64/configs/gki_defconfig   # 应为 =y
```

### 4. 编译

```sh
make ARCH=arm64 LLVM=1 gki_defconfig
make ARCH=arm64 LLVM=1 -j$(nproc) Image
```

## 验证

```sh
# antimark 符号
strings Image | grep -E 'antimark_(handle_cmd|fake_open_check|sysfs_seq_show)'

# susfs 特征（确认 CONFIG_KSU_SUSFS 编进去了）
strings Image | grep -cE 'susfs_[a-z_]+'    # 应 >= 100
```

运行时验证：`antimark_ctl fakefile <目标库> <假库>` 无报错，drmid 检测输出假值。

## 用户态配套（不在本仓库）

- `antimark_ctl`：注册/清理掉包表（子命令 `fakefile` / `fakedel` / `fakeclear` / `token`），走 reboot supercall
- drmid 模块 `mount.sh`：开机读取 `.mode` → `select.sh` 选模板 → `antimark_ctl fakefile` 注册 → `chcon system_lib_file`
- 掉包表路径：`/data/adb/antimark/fake/`；默认 token `{0x0d, 0xf0, 0xfe, 0xba}`
- CMD/OP 定义见 `include/linux/antimark.h`（`CMD_ANTIMARK_SET=0x555d0`，`AM_OP_FILE_FAKE_BEGIN=8` 等）

## 署名

`anykernel.sh` 里 `author=` 改为你自己的署名（当前为 ATK12），打包进刷机包后安装界面会显示。

## 已知坑

1. SUSFS 未开 → 掉包静默失效（见前置条件）
2. CI push 触发会跳过 `if: inputs.xxx` 步骤：push 事件没有 inputs，`if: inputs.susfs_enable` 为 false 时整步被跳过。写成 `if: ${{ inputs.susfs_enable != 'false' }}`
3. 0003 版本混用：6.12 版打 6.6/6.1 编译报错，反之 fuzz 插错位置
4. antimark.c 与 antimark.h 必须同步：内核表结构 / CMD 号变化会导致用户态 ctl 协议失配

## License

GPL-2.0（内核模块）