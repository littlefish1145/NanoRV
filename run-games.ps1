param([string]$Disk='assets/games.raw',[string]$Memory='2G',[ValidateRange(1,8)][int]$Smp=4,[ValidateSet('virgl','2d')][string]$Gpu='virgl',
    [ValidateSet('auto','gdi','vulkan')][string]$DisplayBackend='auto',[ValidateSet('auto','nvidia','intel','amd')][string]$HostGpu='auto',
    [string]$Binary='build/perf21/bin/Release/nanorv.exe')
$ErrorActionPreference='Stop'
if (!(Test-Path (Join-Path $PSScriptRoot $Disk))) {throw 'Game image missing. Use -Disk assets/disk.raw and run nanorv-setup-games inside Debian.'}
& (Join-Path $PSScriptRoot 'run-debian.ps1') -Disk $Disk -Memory $Memory -Smp $Smp -Display window -Gpu $Gpu -DisplayBackend $DisplayBackend -HostGpu $HostGpu -Binary $Binary -Append 'console=ttyS0 earlycon=sbi root=/dev/vda1 rw loglevel=6 systemd.firstboot=no nanorv.games=1'
