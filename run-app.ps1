param(
    [string]$App='xterm', [string[]]$AppArguments=@(),
    [string]$Disk='assets/desktop.raw', [string]$Memory='2G',
    [ValidateRange(1,8)][int]$Smp=4,
    [ValidateRange(1024,65535)][int]$Port=46001,
    [ValidateRange(0,200)][int]$DisplayNumber=0,
    [int]$Timeout=900, [switch]$List, [switch]$Stop,
    [string]$Binary='build/perf21/bin/Release/nanorv.exe'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$sessionDirectory=Join-Path $root 'build/desktop-session'
$sessionFile=Join-Path $sessionDirectory 'session.json'
function Quote-Argument([string]$Value) {
    '"'+([regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1'))+'"'
}
function Request-Desktop($Session,[string]$Route,$Payload=$null) {
    $parameters=@{Uri="http://127.0.0.1:$($Session.port)$Route";Headers=@{Authorization="Bearer $($Session.token)"};TimeoutSec=10}
    if ($null -ne $Payload) {$parameters.Method='Post';$parameters.ContentType='application/json; charset=utf-8';$parameters.Body=[Text.Encoding]::UTF8.GetBytes(($Payload | ConvertTo-Json -Depth 8 -Compress))}
    Invoke-RestMethod @parameters
}
Push-Location $root
try {
    $session=$null
    if (Test-Path -LiteralPath $sessionFile) {
        $candidate=Get-Content -LiteralPath $sessionFile -Raw | ConvertFrom-Json
        $process=Get-Process -Id $candidate.vm_pid -ErrorAction SilentlyContinue
        if ($process -and $process.Path -eq $candidate.binary -and $process.StartTime.ToUniversalTime().Ticks -eq $candidate.started) {$session=$candidate}
    }
    if ($Stop) {
        if (!$session) {Write-Host 'Desktop VM is already stopped.';return}
        Request-Desktop $session '/shutdown' @{} | Out-Null
        try {Wait-Process -Id $session.vm_pid -Timeout 120 -ErrorAction Stop}
        catch {throw 'Guest is still shutting down. Its disk has not been force-closed.'}
        # x410 may have been the user's own instance; only stop one we started.
        if ($session.xserver_pid) {Stop-Process -Id $session.xserver_pid -ErrorAction SilentlyContinue}
        Write-Host 'Desktop VM shut down normally.';return
    }
    if (!$session) {
        if (!(Test-Path -LiteralPath $Disk)) {throw 'Desktop image missing. Run .\scripts\prepare-desktop.ps1 first.'}
        if (!(Test-Path -LiteralPath $Binary)) {
            if ($Binary -ne 'build/perf19/bin/Release/nanorv.exe') {throw "Selected VM binary does not exist: $Binary"}
            & ./scripts/build.ps1 -BuildDir build/perf19 -NoTests
            if ($LASTEXITCODE) {throw 'Build failed'}
        }
        New-Item -ItemType Directory -Force $sessionDirectory | Out-Null
        $random=[System.Security.Cryptography.RandomNumberGenerator]::Create()
        $tokenBytes=New-Object byte[] 32
        $random.GetBytes($tokenBytes);$random.Dispose()
        $token=([BitConverter]::ToString($tokenBytes)).Replace('-','').ToLowerInvariant()
        . ./scripts/x11-session.ps1
        $x11=Start-NanorvX11 $root $DisplayNumber
        $session=[ordered]@{port=$Port;display_number=$x11.display;cookie='';token=$token;binary=(Resolve-Path $Binary).Path}
        $session | ConvertTo-Json | Set-Content -LiteralPath $sessionFile -Encoding utf8
        & python scripts/prepare-desktop-initrd.py --session $sessionFile --output build/desktop-session/initrd.cpio
        if ($LASTEXITCODE) {throw 'Desktop initrd preparation failed'}
        $arguments=@('--bios','assets/fw_jump.bin','--kernel','assets/linux','--initrd','build/desktop-session/initrd.cpio','--disk',$Disk,'--net','user','--forward',"${Port}:46000",'-m',$Memory,'--smp',"$Smp",'--engine','jit','--no-reg-dump','--append','console=ttyS0 earlycon=sbi root=/dev/vda1 rw loglevel=5 systemd.firstboot=no nanorv.desktop=1')
        $vm=Start-Process -FilePath $session.binary -ArgumentList (($arguments | ForEach-Object {Quote-Argument $_}) -join ' ') -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput (Join-Path $sessionDirectory 'console.log') -RedirectStandardError (Join-Path $sessionDirectory 'vm.log') -PassThru
        $session['vm_pid']=$vm.Id;$session['started']=$vm.StartTime.ToUniversalTime().Ticks;$session['xserver_pid']=if($x11.process){$x11.process.Id}else{$null}
        $session | ConvertTo-Json | Set-Content -LiteralPath $sessionFile -Encoding utf8
        Write-Host 'Starting Debian desktop applications...'
    }
    $deadline=[DateTime]::UtcNow.AddSeconds($Timeout)
    $ready=$false
    do {
        $process=Get-Process -Id $session.vm_pid -ErrorAction SilentlyContinue
        if (!$process) {throw 'Desktop VM exited; see build/desktop-session/console.log and vm.log'}
        try {$health=Request-Desktop $session '/health';$ready=$health.ready -and $health.protocol -eq 1} catch {$ready=$false}
        if ($ready) {break}
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    if (!$ready) {throw 'Desktop service is not ready; see build/desktop-session/console.log'}
    if ($List) {Request-Desktop $session '/apps' | Format-Table id,name;return}
    $result=Request-Desktop $session '/launch' @{app=$App;args=@($AppArguments)}
    Write-Host "Launched $App in a Windows window (guest PID $($result.pid))."
} finally {Pop-Location}
