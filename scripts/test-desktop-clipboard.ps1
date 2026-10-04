# Run in a Windows STA PowerShell process. Preserve the user's clipboard formats.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms
$root=Split-Path $PSScriptRoot -Parent
$session=Get-Content (Join-Path $root 'build/desktop-session/session.json') -Raw | ConvertFrom-Json
$headers=@{Authorization="Bearer $($session.token)"}
function Request([string]$Route,$Body=$null) {
    $parameters=@{Uri="http://127.0.0.1:$($session.port)$Route";Headers=$headers;TimeoutSec=10;UseBasicParsing=$true}
    if ($null -ne $Body) {$parameters.Method='Post';$parameters.ContentType='application/json; charset=utf-8';$parameters.Body=[Text.Encoding]::UTF8.GetBytes(($Body | ConvertTo-Json -Compress))}
    Invoke-RestMethod @parameters
}
function Wait-App([int]$GuestPid) {
    $deadline=[DateTime]::UtcNow.AddSeconds(90)
    do {$state=Request "/status/$GuestPid";if (!$state.running) {if ($state.exit_code -ne 0) {throw "Clipboard client exited $($state.exit_code)"};return};Start-Sleep -Milliseconds 250} while ([DateTime]::UtcNow -lt $deadline)
    throw 'Clipboard client timed out'
}
$original=[Windows.Forms.Clipboard]::GetDataObject()
$snapshot=New-Object Windows.Forms.DataObject
if ($original) {
    foreach ($format in $original.GetFormats($false)) {
        $value=$original.GetData($format,$false)
        if ($null -eq $value) {throw 'Cannot snapshot an existing clipboard format; clipboard was left unchanged.'}
        if ($value -is [Drawing.Image]) {$value=$value.Clone()}
        if ($value -is [IO.MemoryStream]) {$value=New-Object IO.MemoryStream(,$value.ToArray())}
        $snapshot.SetData($format,$false,$value)
    }
}
try {
    $fromWindows='NanoRV Windows clipboard'
    [Windows.Forms.Clipboard]::SetText($fromWindows)
    $app=Request '/launch' @{app='xterm';args=@('-e','/bin/sh','-c','test "$(xclip -selection clipboard -o)" = "NanoRV Windows clipboard"')}
    Wait-App $app.pid
    Write-Output 'Windows to Linux clipboard: PASS'
    $fromLinux='NanoRV '+[char]0x4e2d+[char]0x6587+' clipboard'
    $command="printf '%s' '$fromLinux' | xclip -selection clipboard; sleep 1"
    $app=Request '/launch' @{app='xterm';args=@('-e','/bin/sh','-c',$command)}
    $deadline=[DateTime]::UtcNow.AddSeconds(90)
    do {if ([Windows.Forms.Clipboard]::ContainsText() -and [Windows.Forms.Clipboard]::GetText() -eq $fromLinux) {break};Start-Sleep -Milliseconds 250} while ([DateTime]::UtcNow -lt $deadline)
    if ([Windows.Forms.Clipboard]::GetText() -ne $fromLinux) {throw 'Linux to Windows Unicode clipboard did not match'}
    Wait-App $app.pid
    Write-Output 'Linux to Windows Unicode clipboard: PASS'
} finally {
    if ($original) {[Windows.Forms.Clipboard]::SetDataObject($snapshot,$true)} else {[Windows.Forms.Clipboard]::Clear()}
}
