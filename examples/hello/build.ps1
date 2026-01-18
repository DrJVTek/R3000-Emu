param(
    [string]$OutDir = "build"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$cc = (Get-Command mipsel-none-elf-gcc -ErrorAction SilentlyContinue).Source
if (-not $cc)
{
    Write-Error "mipsel-none-elf-gcc not found in PATH. Install a MIPS little-endian toolchain first."
}

$cflags = @(
    "-G0",
    "-O0",
    "-ffreestanding",
    "-fno-builtin",
    "-nostdlib",
    "-fno-pic",
    "-mno-abicalls",
    "-mabi=32",
    "-march=mips1"
)

& $cc @cflags -c ".\start.S" -o "$OutDir\start.o"
& $cc @cflags -c ".\hello.c" -o "$OutDir\hello.o"

& $cc @cflags `
  "-T" ".\link.ld" `
  "-Wl,-Map,$OutDir\hello.map" `
  "$OutDir\start.o" "$OutDir\hello.o" `
  -o "$OutDir\hello.elf"

Write-Host "Built: $OutDir\hello.elf"

