param([string]$Distro = 'Ubuntu-24.04')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    # Already prepared images are left alone, including their users/passwords.
    if ((Test-Path assets/disk.raw) -and (Test-Path assets/debian-initrd.cpio) -and (Test-Path assets/debian-initrd.img)) {
        Write-Host 'Debian assets are already prepared.'; return
    }
    $vms = Get-CimInstance Win32_Process -Filter "name='rvvm.exe' OR name='qemu-system-riscv64.exe'"
    if ($vms | Where-Object { $_.CommandLine -match '(?i)(disk\.raw|debian-reference\.raw)' }) {
        throw 'Stop Debian VMs before preparing/mounting a disk image.'
    }
    $archive = 'assets/debian-13-nocloud-riscv64.tar.xz'
    $manifest = 'assets/debian-SHA512SUMS'
    $mirror = 'https://cloud.debian.org/images/cloud/trixie/latest'
    if (!(Test-Path $manifest)) { Invoke-WebRequest "$mirror/SHA512SUMS" -OutFile $manifest }
    if (!(Test-Path $archive)) {
        Invoke-WebRequest "$mirror/debian-13-nocloud-riscv64.tar.xz" -OutFile "$archive.downloading"
        Move-Item -LiteralPath "$archive.downloading" -Destination $archive
    }
    $entry = Get-Content $manifest | Where-Object { $_ -match '\s+debian-13-nocloud-riscv64\.tar\.xz$' }
    if (!$entry) { throw 'Debian checksum manifest lacks the riscv64 nocloud image.' }
    $expected = ($entry -split '\s+')[0]
    if ((Get-FileHash $archive -Algorithm SHA512).Hash -ne $expected) { throw 'Debian archive SHA-512 mismatch.' }
    foreach ($file in @('assets/linux','assets/initrd.gz','assets/fw_jump.bin')) {
        if (!(Test-Path $file)) { throw "Missing supplied boot asset: $file" }
    }
    & wsl.exe -d $Distro --user root -- python3 scripts/extract-debian.py
    if ($LASTEXITCODE) { throw 'Debian disk/initrd extraction failed.' }
    $gcc = './xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-gcc.exe'
    New-Item -ItemType Directory -Path build -Force | Out-Null
    & $gcc -Os -march=rv64gc -mabi=lp64d -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector '-Wl,-e,_start' '-Wl,--build-id=none' -o build/guest-insmod examples/linux-insmod.c
    if ($LASTEXITCODE) { throw 'Guest module-loader build failed.' }
    & wsl.exe -d $Distro --user root -- python3 scripts/prepare-debian-initrd.py
    if ($LASTEXITCODE) { throw 'Fast Debian initrd preparation failed.' }
    Write-Host 'Prepared Debian. Run .\run-debian.ps1; console login root / rvvm.'
} finally { Pop-Location }
