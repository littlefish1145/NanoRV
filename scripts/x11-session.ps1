# Authenticated X11 transport for the serial Debian VM. Called at launch only.
function Start-RvvmX11([string]$Root) {
    $serverPath=Join-Path $Root 'third_party/vcxsrv/vcxsrv.exe'
    if (!(Test-Path -LiteralPath $serverPath)) {
        & python (Join-Path $Root 'scripts/download-desktop-runtime.py')
        if ($LASTEXITCODE) {throw 'X11 server preparation failed'}
    }
    $ports=@([Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners() | ForEach-Object {$_.Port})
    $display=100
    while ($display -le 200 -and $ports -contains (6000+$display)) {$display++}
    if ($display -gt 200) {throw 'No free X11 display in the supported range'}
    $directory=Join-Path $Root ('build/serial-x11/'+[Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $cookieBytes=New-Object byte[] 16
    $random=[Security.Cryptography.RandomNumberGenerator]::Create()
    try {$random.GetBytes($cookieBytes)} finally {$random.Dispose()}
    $cookie=([BitConverter]::ToString($cookieBytes)).Replace('-','').ToLowerInvariant()
    $sessionPath=Join-Path $directory 'session.json'
    @{display_number=$display;cookie=$cookie} | ConvertTo-Json | Set-Content -LiteralPath $sessionPath -Encoding utf8
    $auth=New-Object IO.MemoryStream
    try {
        $auth.WriteByte(255);$auth.WriteByte(255)
        foreach ($field in @([byte[]]@(),[Text.Encoding]::ASCII.GetBytes("$display"),[Text.Encoding]::ASCII.GetBytes('MIT-MAGIC-COOKIE-1'),$cookieBytes)) {
            $auth.WriteByte([byte]($field.Length -shr 8));$auth.WriteByte([byte]($field.Length -band 255))
            $auth.Write($field,0,$field.Length)
        }
        $authPath=Join-Path $directory 'Xauthority'
        [IO.File]::WriteAllBytes($authPath,$auth.ToArray())
    } finally {$auth.Dispose()}
    $arguments=@(":$display",'-multiwindow','-clipboard','-nowgl','-extension','MIT-SHM','-listen','tcp','-auth',$authPath,'-logfile',(Join-Path $directory 'xserver.log'))
    $quoted=$arguments | ForEach-Object {'"'+([regex]::Replace([regex]::Replace($_,'(\\*)"','$1$1\"'),'(\\+)$','$1$1'))+'"'}
    $server=Start-Process -FilePath $serverPath -ArgumentList ($quoted -join ' ') -WorkingDirectory (Split-Path $serverPath) -WindowStyle Hidden -PassThru
    # The user launch waits for the listening socket; building never starts it.
    for ($attempt=0;$attempt -lt 50;$attempt++) {
        $server.Refresh()
        if ($server.HasExited) {throw "X11 server exited; see $directory/xserver.log"}
        $listeners=[Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
        if ($listeners.Port -contains (6000+$display)) {
            return @{process=$server;session=$sessionPath;initrd=(Join-Path $directory 'initrd.cpio')}
        }
        Start-Sleep -Milliseconds 100
    }
    if (!$server.HasExited) {Stop-Process -Id $server.Id}
    throw "X11 server did not start listening; see $directory/xserver.log"
}
