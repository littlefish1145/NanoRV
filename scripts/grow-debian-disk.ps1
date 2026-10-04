param([string]$Disk = 'assets/disk.raw', [string]$Size = '10G', [string]$Distro = 'Ubuntu-24.04')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    if (!(Test-Path -LiteralPath $Disk)) { throw "Disk image missing: $Disk" }
    $image = if ([IO.Path]::IsPathRooted($Disk)) { $Disk } else { Join-Path $root $Disk }
    $vms = Get-CimInstance Win32_Process -Filter "name='nanorv.exe' OR name='qemu-system-riscv64.exe'"
    if ($vms | Where-Object { $_.CommandLine -match [regex]::Escape($image) }) {
        throw 'Stop VMs using this disk before growing it.'
    }
    $before = (Get-Item -LiteralPath $image).Length
    # WSL understands the repo-relative path directly; anything else needs translating.
    $target = if ($image.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        $image.Substring($root.Length).TrimStart('\', '/') -replace '\\', '/'
    } else {
        (& wsl.exe -d $Distro -- wslpath -a $image).Trim()
        if ($LASTEXITCODE) { throw "wslpath failed for $image" }
    }
    & wsl.exe -d $Distro --user root -- python3 scripts/grow-debian-disk.py --disk $target --size $Size
    if ($LASTEXITCODE) { throw 'Disk growth failed.' }
    $after = (Get-Item -LiteralPath $image).Length
    Write-Host ('Disk grown: {0:N2} GiB -> {1:N2} GiB' -f ($before / 1GB), ($after / 1GB))
} finally { Pop-Location }
