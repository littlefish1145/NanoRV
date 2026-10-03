param([string]$Config = 'Release', [string]$BuildDir = 'build/desktop-smooth', [string]$Compiler = 'cl', [switch]$Asan, [switch]$NoTests)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $targetPath=if([IO.Path]::IsPathRooted($BuildDir)) {$BuildDir} else {Join-Path $root $BuildDir}
    $targetDirectory=[IO.Path]::GetFullPath($targetPath).TrimEnd('\')+'\'
    $active=Get-Process -Name rvvm,rvvm_tests -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path.StartsWith($targetDirectory,[StringComparison]::OrdinalIgnoreCase)
    }
    if ($active) {throw "Build directory is in use by PID $($active.Id -join ', '). Use another -BuildDir or shut down that VM normally first."}
    $envText = & cmd.exe /d /s /c '"D:\VS2022\VC\Auxiliary\Build\vcvars64.bat" >nul && set'
    foreach ($line in $envText) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
    }
    $env:VSLANG = '1033'
    $asanValue=if ($Asan) {'ON'} else {'OFF'}
    $testsValue=if ($NoTests) {'OFF'} else {'ON'}
    & cmake -S . -B $BuildDir -G 'Ninja Multi-Config' "-DCMAKE_CXX_COMPILER=$Compiler" "-DCMAKE_C_COMPILER=$Compiler" "-DRVVM_ASAN=$asanValue" "-DRVVM_BUILD_TESTS=$testsValue"
    if ($LASTEXITCODE) { throw 'CMake configuration failed' }
    # CMake's showIncludes detection is garbled by this installed MSVC locale.
    # Clean first so header edits cannot leave stale, incompatible objects.
    $targets=if ($NoTests) {@('--target','rvvm')} else {@()}
    & cmake --build $BuildDir --config $Config --clean-first @targets
    if ($LASTEXITCODE) { throw 'Build failed' }
} finally { Pop-Location }
