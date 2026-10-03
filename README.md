# RVVM

Windows 原生 RV64GC 系统虚拟机，C++23，使用 **D:/VS2022** 的 MSVC，兼容 clang-cl。
支持 M/S/U、Sv39/Sv48、Zba/Zbb/Zbc/Zbs、标量 AES/SHA 扩展、真实 OpenSBI、VirtIO 磁盘/网卡/显卡/输入、NAT、1–8 个虚拟 CPU、块缓存和自研 x64 JIT。

完整 Debian 13 riscv64 已实际登录，挂载可写 ext4 根文件系统，通过 DHCP/DNS 联网，
成功执行 `apt-get update`、安装和运行 `hello`、用 `curl` 访问 HTTPS。
原始 Linux、安装器 initrd 和 Debian 下载档案均保留。

```powershell
.\run-debian.ps1          # 完整 Debian；默认四核并行；root / rvvm
.\run-debian.ps1 -Smp 1   # 单核
.\run-linux.ps1           # 原先提供的 Linux / BusyBox
.\run-debian.ps1 -Display window  # VirGL 图形与键盘/鼠标
.\run-games.ps1           # 独立 Debian 游戏镜像，四核，自动启动 Neverball
```

Debian 中运行 `poweroff` 正常关机，`reboot` 重新启动。
工作磁盘是 `assets/disk.raw`，退出后软件和文件会保留。虚拟机运行不依赖 WSL 或 QEMU。
启动脚本默认采用 JIT 和在宿主预解压的 initrd；`-Engine interpreter` 可选参考解释器。
`run-debian.ps1 -FullInitrd` 使用保留的完整 Debian initrd，启动更慢。
Debian 内可用 `rvvm-apt-update` 快速刷新已签名的软件源索引；缓存重建延后到普通安装/查询命令按需完成。

构建和检查：

```powershell
.\scripts\build.ps1 -BuildDir build/perf21
ctest --test-dir build/perf21 -C Release --output-on-failure
.\scripts\build.ps1 -Config Debug -BuildDir build/performance-debug
.\scripts\build.ps1 -BuildDir build/performance-clang -Compiler D:/LLVM/bin/clang-cl.exe
python scripts/debian_check.py --timeout 3600
```

此前实验构建 MSVC Release、Debug、clang-cl Release 和 AddressSanitizer 均通过 **199 项测试**。
当前 `perf16` 在浮点访存与区域化代码内存基础上增加 socket 事件唤醒、
libslirp 通知与有界 MMIO 调度，Release 与 AddressSanitizer 均通过 **219 项测试**。
新启动使用 `perf21`，保留宿主 Vulkan/NVIDIA、紧凑原生 TLB 和非递归 RAM 写入锁，增加六寄存器缓存与短循环优化。
Release / AddressSanitizer 均通过 234 项测试，包括启用的 NVIDIA VirGL 和 Vulkan 硬件测试。
CPU 与图形均达到 QEMU 90% 的验收尚未完成，见 [长期目标](docs/QEMU_90_GOAL.md)、[本轮性能记录](docs/PERFORMANCE_21.md)、[QEMU 源码排查](docs/QEMU_SOURCE_AUDIT.md)和[此前寄存器优化](docs/PERFORMANCE_20.md)。
宿主 Vulkan 与客户机软件 Vulkan 的区别见 [Vulkan 支持及性能验收](docs/VULKAN_17.md)。
普通 Debian 串口启动默认接入带认证的 X11；登录后可直接执行已安装的 `gimp`。
纯串口可加 `-NoX11`。GUI 的 D-Bus 会话由 `rvvm-gui` 启动器处理。
完整启动及软件安装属于额外的 guest 集成验证，记录见 [验证文档](docs/VALIDATION.md)。

0.7 实验构建增加代码页字节复核、原生短循环回跳、JIT 统计及可选 NASM SSE2/AVX2
比较函数。构建使用 `./scripts/build.ps1 -BuildDir build/performance-07-profile`，
自动发现 `D:/NASM/nasm.exe`；AVX2 仅在 CPU 与操作系统均支持时启用，保留 SSE2/普通 C++ 回退。
当时的候选在 Linux 基准中仍有回退，旧构建 `build/desktop-smooth` 保留供对照。
最新同工作量 QEMU 对照及计数口径见 [基准剖析](docs/BENCHMARK_ANALYSIS.md)。

0.8 增加跨块原生直接跳转，复用栈帧并保留上下文、代码代数、缓存淘汰与
指令预算检查；缺失/失效链接、异常及 SYSTEM/FENCE 仍走解析器。
构建目录为 `build/performance-08-direct`。`run-debian.ps1 -Binary` 和
`run-app.ps1 -Binary` 可指定这个独立构建；已有 VM 继续运行原来的二进制。


独立桌面应用窗口（类似 WSLg 的使用方式）：

```powershell
.\run-app.ps1 -App xterm
.\run-app.ps1 -App org.xfce.mousepad
.\run-app.ps1 -App pcmanfm
.\run-app.ps1 -List
```

Linux 应用各自显示为 Windows 窗口，支持双向 Unicode 剪贴板；
采用带认证的 X11 + VcXsrv，CPU 由 RVVM 执行。
新版已改善设备与网络等待延迟。已有桌面会话继续复用原进程；保存文件后
运行 `.\run-app.ps1 -Stop`，再启动应用即可使用新版。
使用与范围见 [DESKTOP.md](docs/DESKTOP.md)。

第一次准备新的官方镜像时运行 `scripts/prepare-debian.ps1`。准备阶段需要已有 WSL
Ubuntu 的 Python、cpio、initramfs-tools、mount、openssl，以及随项目提供的 RISC-V GCC。
已有工作镜像不会被此脚本覆盖。当前工作区已经准备好，可直接启动。

命令行也支持 ELF/平坦裸机程序、trace、指令预算、外部 DTB、raw 磁盘。
qcow2 需先用 `qemu-img convert -O raw` 转换。网络采用用户模式 NAT，`--forward 主机端口:客户机端口` 提供仅绑定 Windows 本机的 TCP 转发。
SMP 每个 hart 使用独立宿主线程，拥有独立 CPU/CSR/TLB/代码缓存；两个 JIT 编译线程由所有 hart 共享。
共享 RAM、AMO、LR/SC、页表 A/D 和 DMA 支持并发；设备由 I/O 线程处理。
JIT 通过独立 IR、常量折叠/死赋值消除/连续更新合并、块内寄存器分配和原生块链接编译整数与控制流。
软件 TLB、RAM 快速路径、分支目标缓存、MMIO 页面缓存和代码页代数检查减少派发开销。
SSE2 加速常用浮点运算；其余复杂指令使用共享参考实现。
CPU 执行不使用 QEMU TCG。图形使用 Windows WGL + VirGL 调用宿主 OpenGL 驱动，已实测使用 RTX 5060。
VirtIO 图形窗口可通过 Windows Vulkan 在 NVIDIA/Intel/AMD 硬件上上传、缩放和呈现帧缓冲；
`--display-backend vulkan --host-gpu nvidia` 强制选择 NVIDIA，`auto` 优先独显并保留 GDI 回退。
客户机 Vulkan 仍走 Mesa lavapipe 软件渲染，尚未提供 Venus/客户机 Vulkan 命令转发。
游戏使用说明见 [GAMES.md](docs/GAMES.md)。RVV/H 和完整 ISA 认证仍属后续扩展。

本轮完整 Debian 登录实测从旧版单核 **288.25 s** 降至新版四核 **147.14 s**。
四核启动/重启、APT 与 HTTPS 检查通过；总纲性能清单、固定工作量基准和局限见 [PERFORMANCE.md](docs/PERFORMANCE.md)。
`-SmpMode cooperative` 保留原分时调度，trace 使用该模式；生产脚本使用独立的新版构建目录。

设计与使用：[总纲](docs/MASTER_PLAN.md)、[启动说明](docs/LINUX_BOOT.md)、
[实现状态](docs/ISA_STATUS.md)、[架构](docs/ARCHITECTURE.md)。
SoftFloat 和网络运行库的许可证及来源保存在 `third_party/` 中。

