# X11 transport for the serial Debian VM, provided by the installed x410 server.
# x410 serves display :0 over TCP and does not require an X cookie, so the guest
# only needs DISPLAY=10.0.2.2:0 and no Xauthority file.
function Get-NanorvX410Path {
    $alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\x410.exe'
    if (Test-Path -LiteralPath $alias) { return $alias }
    $package = Get-AppxPackage -Name 'ChoungNetworksUS.X410S' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($package) {
        $server = Join-Path $package.InstallLocation 'X410\X410.exe'
        if (Test-Path -LiteralPath $server) { return $server }
    }
    foreach ($candidate in @("$env:LOCALAPPDATA\Programs\X410\X410.exe", 'C:\Program Files\X410\X410.exe')) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'x410 is not installed. Install X410 (Microsoft Store or the standalone installer) and retry.'
}
function Test-NanorvX11Port([int]$Display) {
    return @(Get-NetTCPConnection -State Listen -LocalPort (6000 + $Display) -ErrorAction SilentlyContinue).Count -gt 0
}
function Start-NanorvX11([string]$Root, [int]$Display = 0) {
    $server = Get-NanorvX410Path
    $process = $null
    if (!(Test-NanorvX11Port $Display)) {
        # Store builds ignore command line switches, so start the default server
        # and let it claim :0; it enables TCP and needs no cookie on its own.
        Start-Process -FilePath $server | Out-Null
        for ($attempt = 0; $attempt -lt 600 -and !(Test-NanorvX11Port $Display); $attempt++) {
            Start-Sleep -Milliseconds 100
        }
        if (!(Test-NanorvX11Port $Display)) { throw "x410 did not start listening on display :$Display" }
        $process = Get-Process X410 -ErrorAction SilentlyContinue | Sort-Object StartTime | Select-Object -Last 1
    }
    $directory = Join-Path $Root ('build/serial-x11/' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $sessionPath = Join-Path $directory 'session.json'
    @{display_number = $Display; cookie = ''} | ConvertTo-Json | Set-Content -LiteralPath $sessionPath -Encoding utf8
    # Only a process this function started may be stopped by the caller.
    return @{process = $process; session = $sessionPath; initrd = (Join-Path $directory 'initrd.cpio'); display = $Display}
}
