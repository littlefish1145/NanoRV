param([string]$SourceDisk='assets/games.raw',[string]$Disk='assets/desktop.raw')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    if (!(Test-Path -LiteralPath $Disk)) {
        # Holding this stream prevents another VM from opening the source for writing.
        $source=[IO.File]::Open((Join-Path $root $SourceDisk),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        try {
            $destination=[IO.File]::Open((Join-Path $root $Disk),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
            try {$source.CopyTo($destination);$destination.Flush($true)} finally {$destination.Dispose()}
        } finally {$source.Dispose()}
    }
    & python scripts/prepare-desktop-initrd.py --output build/debian-desktop-initrd.cpio
    if ($LASTEXITCODE) {throw 'Desktop initrd creation failed'}
    # QEMU is a provisioning tool only. run-app.ps1 executes the native NanoRV CPU.
    & python scripts/debian_check.py --qemu-wsl --disk $Disk --initrd build/debian-desktop-initrd.cpio --timeout 1800 --log build/desktop-install.log --command nanorv-setup-desktop
    if ($LASTEXITCODE) {throw 'Desktop package installation failed; see build/desktop-install.log'}
    Write-Host 'Ready: .\run-app.ps1 -App xterm'
} finally {Pop-Location}
