param(
    [string]$Disk = 'assets/disk.raw',
    [string]$Memory = '1G',
    [ValidateRange(1,8)][int]$Smp = 4,
    [ValidateSet('parallel','cooperative')][string]$SmpMode = 'parallel',
    [ValidateSet('jit','block','interpreter')][string]$Engine = 'jit',
    [ValidateRange(0,8)][int]$JitWorkers = 2,
    [ValidateSet('none','window','headless')][string]$Display = 'none',
    [ValidateSet('2d','virgl')][string]$Gpu = 'virgl',
    [ValidateSet('auto','gdi','vulkan')][string]$DisplayBackend = 'auto',
    [ValidateSet('auto','nvidia','intel','amd')][string]$HostGpu = 'auto',
    [string]$HttpProxy = '',
    [string]$Append = 'console=ttyS0 earlycon=sbi root=/dev/vda1 rw loglevel=6 systemd.firstboot=no',
    [switch]$FullInitrd,
    [switch]$NoX11,
    [string]$Binary = 'build/perf22/bin/Release/rvvm.exe'
)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
$x11=$null
try {
    $binaryPath = if ([IO.Path]::IsPathRooted($Binary)) { $Binary } else { Join-Path $PSScriptRoot $Binary }
    if (!(Test-Path -LiteralPath $binaryPath)) {
        if ($Binary -notmatch '^build[\\/]([^\\/]+)') { throw "Selected VM binary does not exist: $binaryPath" }
        & ./scripts/build.ps1 -BuildDir (Join-Path 'build' $Matches[1]) -NoTests
    }
    if (!$FullInitrd -and !(Test-Path assets/debian-performance-initrd.cpio)) { & ./scripts/prepare-performance.ps1 }
    $initrd = if ($FullInitrd) { 'assets/debian-initrd.img' } else { 'assets/debian-performance-initrd.cpio' }
    if (!(Test-Path -LiteralPath $Disk) -or !(Test-Path -LiteralPath $initrd)) {
        throw 'Debian assets missing. Run .\scripts\prepare-debian.ps1 once; see docs/LINUX_BOOT.md.'
    }
    Write-Host 'Debian serial console: root / rvvm. Type poweroff to shut down.'
    $extra=@()
    if ($DisplayBackend -eq 'vulkan' -and $Display -ne 'window') {throw '-DisplayBackend vulkan requires -Display window'}
    if ($Display -ne 'none') {
        $extra+=@('--display',$Display,'--gpu',$Gpu)
        if ($DisplayBackend -ne 'auto') { $extra+=@('--display-backend',$DisplayBackend) }
        if ($HostGpu -ne 'auto') { $extra+=@('--host-gpu',$HostGpu) }
        $Append+=' rvvm.graphics=1'
    }
    if ($HttpProxy) {
        if ($HttpProxy -match '[\s"\\]') { throw 'HTTP proxy must be a URL without whitespace or quotes.' }
        $Append+=" rvvm.http_proxy=$HttpProxy"
    }
    if (!$NoX11 -and $Display -eq 'none') {
        . ./scripts/x11-session.ps1
        $x11=Start-RvvmX11 $PSScriptRoot
        $base=if ($FullInitrd) {'assets/debian-initrd.cpio'} else {$initrd}
        & python scripts/prepare-desktop-initrd.py --serial-x11 --session $x11.session --base $base --output $x11.initrd
        if ($LASTEXITCODE) {throw 'Serial X11 initrd preparation failed'}
        $initrd=$x11.initrd;$Append+=' rvvm.x11=1'
        Write-Host 'GUI applications can be launched here with commands such as gimp or xterm.'
    }
    & $binaryPath --bios assets/fw_jump.bin --kernel assets/linux --initrd $initrd --disk $Disk --net user -m $Memory --smp $Smp --smp-mode $SmpMode --engine $Engine --jit-workers $JitWorkers --append $Append --no-reg-dump @extra
    exit $LASTEXITCODE
} finally {
    if ($x11) {$x11.process.Refresh();if (!$x11.process.HasExited) {Stop-Process -Id $x11.process.Id}}
    Pop-Location
}
