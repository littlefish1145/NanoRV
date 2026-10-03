# Builds an RV64GC demo guest program with the LLVM cross toolchain.
# No WSL and no RISC-V SDK required: clang can target riscv64 and lld links it.
#
#   .\build.ps1
#   .\build.ps1 -Source atomics.S -Output atomics.elf -March rv64ima

param(
    [string]$Clang = "clang",
    [string]$Source = "program.S",
    [string]$Output = "program.elf",
    [string]$March = "rv64i"
)

$ErrorActionPreference = "Stop"

$arguments = @(
    "--target=riscv64-unknown-elf",
    "-march=$March",
    "-mabi=lp64",
    "-nostdlib",
    "-fuse-ld=lld",
    "-Wl,--image-base=0x80000000",
    "-o", $Output,
    $Source
)

& $Clang @arguments
if ($LASTEXITCODE -ne 0) {
    throw "assembler failed with exit code $LASTEXITCODE"
}
Write-Host "wrote $Output"