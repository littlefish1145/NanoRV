param([string]$Source = 'assets/initrd.gz', [string]$Destination = 'assets/initrd.cpio')
$ErrorActionPreference = 'Stop'
$inputPath = Join-Path (Split-Path $PSScriptRoot -Parent) $Source
$outputPath = Join-Path (Split-Path $PSScriptRoot -Parent) $Destination
if ((Test-Path -LiteralPath $outputPath) -and (Get-Item -LiteralPath $outputPath).LastWriteTimeUtc -ge (Get-Item -LiteralPath $inputPath).LastWriteTimeUtc) { return }
$inputFile = [IO.File]::OpenRead($inputPath)
try {
    $gzip = [IO.Compression.GZipStream]::new($inputFile, [IO.Compression.CompressionMode]::Decompress)
    try {
        $outputFile = [IO.File]::Create("$outputPath.tmp")
        try { $gzip.CopyTo($outputFile) } finally { $outputFile.Dispose() }
    } finally { $gzip.Dispose() }
} finally { $inputFile.Dispose() }
Move-Item -LiteralPath "$outputPath.tmp" -Destination $outputPath -Force
