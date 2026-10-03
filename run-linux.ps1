param([string]$Append = 'console=ttyS0 earlycon=sbi rdinit=/bin/sh TERM=dumb', [string]$Memory = '512M',
    [ValidateSet('jit','block','interpreter')][string]$Engine = 'jit', [switch]$OriginalInitrd)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $binary = Join-Path $PSScriptRoot 'build/perf21/bin/Release/rvvm.exe'
    if (!(Test-Path -LiteralPath $binary)) { & ./scripts/build.ps1 -BuildDir build/perf21 -NoTests }
    $initrd = 'assets/initrd.gz'
    if (!$OriginalInitrd) { & ./scripts/prepare-initrd.ps1; $initrd = 'assets/initrd.cpio' }
    & $binary --bios assets/fw_jump.bin --kernel assets/linux --initrd $initrd -m $Memory --engine $Engine --append $Append --no-reg-dump
    exit $LASTEXITCODE
} finally { Pop-Location }
