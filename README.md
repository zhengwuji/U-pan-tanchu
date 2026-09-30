# 强制弹出优盘 (Force Eject USB)

[![自动编译发布](https://github.com/zhengwuji/U-pan-tanchu/actions/workflows/build.yml/badge.svg)](https://github.com/zhengwuji/U-pan-tanchu/actions/workflows/build.yml)

专用于**强制弹出 / 卸载** U 盘、移动硬盘、光驱等可移动设备的 Windows 原创小工具。

- 🟢 绿色单文件，静态编译，无任何依赖
- 💪 五级强制策略：占用再狠也能弹（自动关闭占用进程的句柄）
- 🏷️ 自动识别设备类型：U 盘 / USB 移动硬盘 / USB 光驱 / 内置光驱 / 虚拟光驱 / 存储卡……全部明确标注
- 🖥️ 托盘模式 + 命令行模式双支持
- 🇨🇳 简体中文界面与日志

> 本程序为全新编写的原创代码，仅借鉴 Windows 公开 API 的设备管理行为。

## 📥 下载

前往 [**Releases 页面**](https://github.com/zhengwuji/U-pan-tanchu/releases) 下载最新的 `强制弹出优盘.exe`。

每次代码更新都会**自动编译并发布**新版本到 Releases（见下方 [CI 自动构建](#-ci-自动构建)）。

## 🚀 使用方法

### 托盘模式（推荐日常使用）

1. 双击 `强制弹出优盘.exe`（需要管理员权限，会弹 UAC 确认框）；
2. 任务栏托盘出现图标（Windows 11 默认收在托盘溢出区 ^ 里）；
3. **左键**点击图标 → 菜单列出所有可弹出设备（带类型标注）→ 点选即强制弹出；
4. **右键**点击图标 → 弹出全部设备 / 退出。

弹出的结果会以气泡通知显示。

### 命令行模式

| 命令 | 作用 |
|------|------|
| `强制弹出优盘.exe E:` | 强制弹出盘符 E: 所在的整块设备（U盘/移动硬盘/光驱） |
| `强制弹出优盘.exe all` | 弹出全部 USB / 可移动设备（含 USB 光驱） |
| `强制弹出优盘.exe -l` | 列出所有设备（标明类型、总线、盘符） |
| `强制弹出优盘.exe -f E:` | 允许对内置设备（内置光驱/硬盘/虚拟盘）也弹出（系统盘除外） |
| `强制弹出优盘.exe -h` | 显示帮助 |

也支持按卷 GUID（`\\?\Volume{...}`）或 `\\.\PhysicalDriveN` 指定目标。

`-l` 输出示例：

```
--- 设备列表 (6 个) ---
磁盘 4  [内置硬盘]  总线:NVMe  ZHITAI TiPlus5000 1TB
    盘符: D:    卷数: 1    [内置设备, 需 -f 才可弹出]
    设备实例: SCSI\DISK&VEN_NVME&PROD_ZHITAI_TIPLUS500\5&2578EE34&0&000000
光驱 0  [虚拟光驱]  总线:VHD  Microsoft 虚拟 DVD-ROM
    盘符: H:    卷数: 1    [内置设备, 需 -f 才可弹出]
    设备实例: SCSI\CdRom&Ven_Msft&Prod_Virtual_DVD-ROM\2&1F4ADFFE&0&000002
```

## 🏷️ 设备类型标注

| 菜单/列表标注 | 判定依据 |
|---------------|----------|
| **U 盘** | USB 总线 + 介质可移动 |
| **USB 移动硬盘** | USB 总线 + 介质固定 |
| **USB 光驱** | 光驱设备类 + USB 总线 |
| **内置光驱** | 光驱设备类 + 非 USB 总线 |
| **虚拟光驱** | 光驱设备类 + 文件虚拟总线（挂载的 ISO） |
| **存储卡** | SD / MMC 总线 |
| **1394 磁盘** | FireWire 总线 |
| **虚拟磁盘** | 文件虚拟总线（VHD 等） |
| **可移动磁盘** | 其它带可移动属性的设备 |
| **内置硬盘** | 固定介质 + 非 USB 总线（需 `-f` 才可弹出） |

## ⚙️ 强制弹出策略（五级递进）

1. **刷盘 + 锁卷 + 卸载卷**：`FlushFileBuffers` + `FSCTL_LOCK_VOLUME` + `FSCTL_DISMOUNT_VOLUME`；
2. **强制关闭占用句柄**：用 `NtQuerySystemInformation(SystemExtendedHandleInformation)` 枚举全系统句柄，通过 `DuplicateHandle(DUPLICATE_CLOSE_SOURCE)` 关闭其它进程中占用目标设备的文件/卷句柄，然后重试；
3. **正规弹出**：`CM_Request_Device_EjectW`（被占用否决时继续关句柄重试）；
4. **介质弹出兜底**：`IOCTL_STORAGE_EJECT_MEDIA`（带"设备是否真的消失"验证，不虚报成功）；
5. **最后手段**：卷已全部卸载但驱动仍拒绝弹出时，`SetupDiRemoveDevice` 直接移除设备节点。

## 🛡️ 安全保护

- **永远拒绝弹出 Windows 系统卷所在的物理磁盘**；
- **永远拒绝弹出本程序自己所在的磁盘**；
- 默认只弹 USB / 可移动设备，内置设备和虚拟设备需显式加 `-f`；
- 弹出前所有卷先刷盘卸载，数据已保存。

## ⚠️ 注意事项

- **强制模式会关闭其它进程中占用目标设备的句柄**，相关程序对已打开文件的读写会失败——弹出前请先在其它程序里保存文件；
- 需要**管理员权限**运行（manifest 已声明，双击会弹 UAC）；
- 每次运行都会在 exe 同目录写 `<exe名>.log` 日志（UTF-8），排查问题直接看日志。

## 📋 退出码

| 码 | 含义 |
|----|------|
| 0 | 成功 |
| 1 | 参数错误 |
| 2 | 弹出失败 / 拒绝弹出 |
| 3 | 未找到目标设备 |

## 🔨 本地构建

需要 MinGW-w64 (GCC ≥ 13)。把工具链放在 `..\_toolchain\mingw64` 后运行：

```bat
build.bat
```

也可以手动执行（任何 MinGW-w64 均可）：

```bat
windres force.rc -O coff -o force_res.o
g++ -O2 -std=c++17 -municode -mwindows -static -finput-charset=UTF-8 -fwide-exec-charset=UTF-16LE main.cpp force_res.o -o ForceEjectUSB.exe -luser32 -lshell32 -lsetupapi -lcfgmgr32
```

## 🤖 CI 自动构建

仓库配置了 GitHub Actions（[.github/workflows/build.yml](.github/workflows/build.yml)）：

- **每次代码新增 / 修改并推送时自动触发编译**（改动 `README.md` 不会触发）；
- 使用 MSYS2 MinGW-w64 工具链静态编译；
- 编译产物自动**发布到 Releases**，附 `RELEASE_NOTES.md` 中的中文更新内容。

## 📝 更新日志

### v1.1（2026-09-30）

- ✅ 新增光驱支持：同时枚举磁盘与光驱（CD-ROM）设备类，USB 光驱可直接弹出，内置/虚拟光驱加 `-f` 可弹；
- ✅ 新增设备类型明确标注：所有设备按 总线类型 + 介质属性 自动识别为 **U 盘 / USB 移动硬盘 / USB 光驱 / 内置光驱 / 虚拟光驱 / 存储卡 / 1394 磁盘 / 虚拟磁盘 / 内置硬盘**，托盘菜单、`-l` 列表、弹出结果全部显示类型；
- ✅ `-l` 列表升级：显示 设备类别 + 编号 + [类型] + 总线 + 友好名称 + 盘符 + 卷数 + 弹出许可，总线名精确到 SATA / NVMe / USB / SD 等；
- ✅ 托盘菜单、右键菜单、结果气泡全部带上类型标注；
- ✅ 卷与设备匹配改为 物理编号 + 设备栈类型 双重校验，磁盘和光驱编号不会混淆。

### v1.0（2026-09-30）

- 🎉 首个版本发布；
- ✅ 托盘模式：左键选设备弹出、右键弹出全部/退出，结果气泡通知；
- ✅ 命令行模式：按盘符 / 卷 GUID / PhysicalDrive 弹出，`all` 弹出全部，`-l` 列表，`-f` 解锁内置设备；
- ✅ 五级强制弹出策略（刷盘卸载 → 强制关占用句柄 → 正规弹出 → 介质弹出兜底 → 设备节点移除）；
- ✅ 系统盘 / 程序所在盘双重保护；
- ✅ 全程 UTF-8 日志（exe 同目录 `.log`）。
