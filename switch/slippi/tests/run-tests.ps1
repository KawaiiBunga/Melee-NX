# Builds and runs the Slippi protocol host tests with a Windows clang (llvm-mingw).
#   powershell -ExecutionPolicy Bypass -File switch\slippi\tests\run-tests.ps1 [-Clang path\to\clang++.exe]
param([string]$Clang = "E:\tools\llvm-mingw\llvm-mingw-20261006-ucrt-x86_64\bin\clang++.exe")
$ErrorActionPreference = "Stop"
$S = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent $S)
$out = Join-Path $repo "build\slippi-tests"
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out "slippi-protocol-tests.exe"
& $Clang -std=c++20 -O1 -Wall "-I$S" "-I$repo\launcher\include" (Join-Path $S "slippi_protocol.cpp") `
    (Join-Path $S "tests\protocol_tests.cpp") -o $exe -static
if ($LASTEXITCODE) { throw "building the tests failed" }
& $exe
if ($LASTEXITCODE) { throw "tests failed" }
