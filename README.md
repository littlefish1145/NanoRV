```text
   _  _            _   ___  __     __
  | \| | __ _ _ _ | | | _ \ \ \   / /
  | .` |/ _` | ' \| | |   /  \ \ / /
  |_|\_|\__,_|_||_|_| |_|_\   \_V_/
              N A N O R V
```
# NanoRV

A native Windows system emulator for RISC-V, written in C++23. It runs unmodified
Linux and RISC-V binaries on Windows without QEMU, WSL or a hypervisor: an
x64 JIT executes guest code, VirGL renders OpenGL through the host GPU, and
VirtIO devices provide disk, network, graphics and input.

A full Debian 13 (riscv64) userland boots on it, including systemd, a writable
ext4 root filesystem, NAT networking with DNS, `apt` over HTTPS and persistent
cross-process disk state.

```
riscv,isa = "rv64imafdc_zicsr_zifencei_zba_zbb_zbc_zbs_zbkb_zbkc_zkne_zknd_zknh"
```

## Features

| Area | Notes |
|---|---|
| CPU | RV64 I/M/A/C, F/D via Berkeley SoftFloat, 1-8 independent vCPU threads |
| Extensions | Zba, Zbb, Zbc, Zbs, Zbkb, Zbkc, Zkne, Zknd, Zknh, Zicsr, Zifencei |
| Privilege | M/S/U modes, delegation, traps, PMP16, MRET/SRET/WFI/SFENCE.VMA |
| MMU | Sv39 and Sv48, 3/4-level page tables, superpages, A/D bits, SUM/MXR/MPRV |
| Devices | VirtIO block, net (libslirp NAT + DHCP/DNS), GPU, input; CLINT, PLIC, UART, Goldfish RTC |
| Graphics | 2D scanout plus VirGL OpenGL on the host WGL renderer; optional Vulkan host presenter |
| Engines | reference interpreter, block engine, and an x64 JIT with its own IR and optimizer |
| SMP | 1-8 host threads with shared RAM atomics, AMO/LR/SC, PTE A/D handling and DMA concurrency |
| Host X11 | guest GUI programs are forwarded to an installed X410 server on display `:0` |
| Lifecycle | OpenSBI firmware, in-process reboot, syscon poweroff, non-zero exit on guest failure |

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 (the scripts expect `D:\VS2022`) or clang-cl
- CMake and Ninja Multi-Config
- Optional: NASM (`D:/NASM`) for hand-written SSE2/AVX2 helpers; the build falls
  back to portable C++ when it is absent
- WSL (Ubuntu) for the one-time image preparation scripts
- X410 from the Microsoft Store (or the standalone installer) for the host-side
  X display used by `run-debian.ps1` and `run-app.ps1`

## Build

```powershell
.\scripts\build.ps1 -BuildDir build/nanorv
ctest --test-dir build/nanorv -C Release --output-on-failure
```

Variants:

```powershell
.\scripts\build.ps1 -Config Debug -BuildDir build/nanorv-debug
.\scripts\build.ps1 -BuildDir build/nanorv-clang -Compiler D:/LLVM/bin/clang-cl.exe
.\scripts\build.ps1 -BuildDir build/nanorv-asan -Asan
```

Products land in `<build>/bin/Release/`: `nanorv.exe`, `nanorv_tests.exe`,
`nanorv_jit_inspect.exe`.

## Guest images

This repository contains the emulator source and the vendored OpenSBI firmware
only. Disk images, the guest kernel and the initramfs are produced locally and
are listed in `.gitignore`, so a fresh clone starts with no multi-gigabyte
blobs.

Everything needed for a Debian guest comes out of the official cloud image, so
there is no image to convert by hand:

```powershell
.\scripts\prepare-debian.ps1
```

That script performs the following, and skips any step whose output already
exists (your users, packages and passwords are never touched):

1. Downloads `debian-13-nocloud-riscv64.tar.xz` and `SHA512SUMS` from
   `cloud.debian.org/images/cloud/trixie/latest`, if they are missing.
2. Verifies the archive against the published SHA-512 and refuses to continue on
   a mismatch.
3. Extracts `disk.raw` from the archive. It is already a plain raw image with a
   GPT and an ext4 root filesystem, so no conversion step is required.
4. Mounts that image read-only and copies the boot files out of it:
   `/boot/vmlinux-*` becomes `assets/linux` (Debian ships the bare RV64 Image
   under that name on this architecture) and `/boot/initrd.img-*` becomes
   `assets/debian-boot-initrd.cpio`. Both therefore always match the kernel
   inside the disk.
5. On the very first run, applies `scripts/configure-debian.py` to the new image:
   hostname, APT layout, serial console and the `root`/`rvvm` credentials.
6. Builds the small boot initrd with `scripts/prepare-debian-initrd.py`: BusyBox,
   the five required VirtIO/ext4 modules and a `switch_root` into the real root.

Running a Debian guest later only needs `assets/disk.raw` plus the generated
initrd; `run-debian.ps1` calls `scripts/prepare-performance.ps1` automatically if
`assets/debian-performance-initrd.cpio` is absent.

Resulting local-only files:

| Path | Contents |
|---|---|
| `assets/disk.raw` | guest root disk (raw, GPT + ext4) |
| `assets/linux` | RV64 Linux Image |
| `assets/debian-boot-initrd.cpio` | initramfs as published by Debian |
| `assets/debian-initrd.cpio` | generated fast initrd |
| `assets/debian-performance-initrd.cpio` | generated initrd with the bundled game |

To use an image you already have, convert it to raw first — `--disk` accepts
nothing else:

```powershell
qemu-img convert -O raw guest.qcow2 assets/disk.raw
```

## Run

### Debian (recommended)

```powershell
.\run-debian.ps1                 # console login: root / rvvm
```

`run-debian.ps1` makes sure an X410 server is available for the guest GUI (it
reuses the one you already have running, or starts it). Inside Debian, launch
GUI programs through the wrapper so they pick up `DISPLAY=10.0.2.2:0`:

```sh
nanorv-gui gimp          # /usr/local/bin/gimp is a wrapper for this
nanorv-gui xterm
```

Type `poweroff` to shut down and `reboot` to restart in place.

Useful options:

```powershell
.\run-debian.ps1 -Smp 8 -Memory 8G          # more vCPUs and RAM
.\run-debian.ps1 -Display window            # VirtIO GPU window (guest Xorg + VirGL)
.\run-debian.ps1 -Display window -Gpu 2d    # 2D only
.\run-debian.ps1 -DisplayBackend vulkan      # host presentation through Vulkan
.\run-debian.ps1 -HostGpu nvidia             # require a specific host GPU vendor
.\run-debian.ps1 -NoX11                      # do not start a host X server
.\run-debian.ps1 -Engine interpreter         # reference interpreter
.\run-debian.ps1 -HttpProxy http://host:port
```

### BusyBox Linux

```powershell
.\run-linux.ps1
```

### Games and desktop applications

```powershell
.\run-games.ps1              # Debian image with Xorg + Neverball
.\run-app.ps1 -App xterm     # launch a guest app straight into a Windows window
.\run-app.ps1 -List
.\run-app.ps1 -Stop
```

`run-games.ps1` boots a disk prepared with `rvvm-setup-games`
(`scripts/setup-games.sh`), which installs Xorg, Mesa and the game.

## Command line

```
nanorv run <image.elf|image.bin> [options]
nanorv <image.elf|image.bin> [options]
```

| Option | Meaning |
|---|---|
| `--bios FILE` | OpenSBI firmware (`assets/fw_jump.bin`) |
| `--kernel FILE` | RV64 Linux Image to boot |
| `--initrd FILE` | initramfs handed to the kernel |
| `--disk FILE` | writable **raw** disk on the VirtIO block device |
| `--net user` | NAT networking with DHCP and DNS |
| `--forward H:G` | forward host TCP port `H` to guest port `G` |
| `--display MODE` | `window` or `headless`; enables graphics and input |
| `--gpu TYPE` | `2d` or `virgl` |
| `--display-backend` | `auto`, `gdi` or `vulkan` (presentation only) |
| `--host-gpu TYPE` | `auto`, `nvidia`, `intel`, `amd` |
| `--screenshot FILE` | save the final framebuffer as a BMP |
| `--host-vulkan-info` | list host Vulkan devices and exit |
| `--host-vulkan-test` | verify host Vulkan upload/readback and exit |
| `--dtb FILE` | use an external device tree instead of the generated one |
| `--append TEXT` | kernel command line |
| `--elf` / `--raw` | image format (ELF64 is the default) |
| `--load-addr ADDR`, `--entry ADDR` | placement for `--raw`, entry override |
| `-m, --memory SIZE` | RAM size, accepts `K`/`M`/`G` suffixes |
| `--smp N`, `--smp-mode MODE` | 1-8 harts; `parallel` (default) or `cooperative` |
| `--engine NAME` | `interpreter`, `block` or `jit` |
| `--jit-workers N` | shared JIT compiler threads (SMP only) |
| `-d, --trace` | print an execution trace |
| `--max-steps N` | stop after N instructions |

`--disk` opens the image read/write and exclusively, so only one VM may use a
given image at a time. Only raw images are supported; convert qcow2 with
`qemu-img convert -O raw`.

## Disk images

The guest disk is a plain raw image with a GPT and ext4, both owned by the
guest. To enlarge one offline (VM stopped, WSL required):

```powershell
.\scripts\grow-debian-disk.ps1 -Size 20G
```

It extends the image file, grows the root partition with `sfdisk`, then runs
`e2fsck` and `resize2fs`. The operation is idempotent and keeps all data.

## Repository layout

```
src/            CPU, MMU, devices, machine model, platform backends
  cpu/          decoder, reference semantics, IR, x64 JIT
  mmu/          Sv39/Sv48 address translation and PMP
  devices/      VirtIO block/net/GPU/input, CLINT/PLIC/UART/RTC
  machine/      configuration, FDT generation, run loop, SMP
  platform/     Win32 disk, network, display, renderer and Vulkan code
tests/          unit, differential and graphics tests
scripts/        build, image preparation and launch helpers
third_party/    SoftFloat sources, libslirp/VirGL/Vulkan headers and runtime libs
assets/         vendored OpenSBI firmware; disks and boot files are local-only
docs/           developer notes (not tracked by git)
```

Only `assets/fw_*.bin|elf`, `assets/opensbi.deb` and `assets/usr/**` (the
OpenSBI package payload) are tracked under `assets/`. Everything else there is
produced by `scripts/prepare-debian.ps1`.

## Known limitations

- No RVV (vector) or H (hypervisor) extension.
- Guest Vulkan runs on Mesa lavapipe in software; there is no Venus or command
  forwarding to the host GPU.
- Disk images must be raw; there is no built-in qcow2 layer.
- The serial console shell is started with `--noprofile --norc`, so `DISPLAY` is
  only set for programs launched through `nanorv-gui`.
- No GDB remote stub, no snapshots.
- Memory ordering is stronger than RVWMO (consistent 64-bit atomics plus
  sharded locks), which is observable in ordering tests.

## Development notes

- `nanorv_tests.exe` runs 235 unit, differential and graphics tests; the same
  suite is wired into `ctest`.
- `docs/` holds the design, performance and validation notes. It is ignored by
  git on purpose: it is a working log, not part of the shipped source.
- Guest-visible names (`riscv,isa`, DTB model `NanoRV virt`, kernel command line
  flags such as `nanorv.graphics=1`) follow the NanoRV project name.
