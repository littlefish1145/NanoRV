param([string]$Distro='Ubuntu-24.04')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    if (!(Test-Path assets/debian-initrd.cpio)) { & ./scripts/prepare-debian.ps1 -Distro $Distro }
    New-Item -ItemType Directory -Force build | Out-Null
    & ./xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-gcc.exe -O2 -march=rv64gc_zba_zbb_zbs -mabi=lp64d -nostdlib -ffreestanding -fno-builtin '-Wl,-e,_start' '-Wl,--build-id=none' examples/linux-breakout.c -o build/nanorv-breakout
    if ($LASTEXITCODE) { throw 'Bundled game compilation failed' }
    & wsl.exe -d $Distro --user root -- python3 scripts/prepare-performance-initrd.py
    if ($LASTEXITCODE) { throw 'Performance initrd preparation failed' }
} finally { Pop-Location }
