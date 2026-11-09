# Builds and runs the Slippi host tests with a Windows clang (llvm-mingw): the protocol tests, the
# host command tests, and the loopback network tests (real ENet over 127.0.0.1, using ENet's win32 backend).
#   powershell -ExecutionPolicy Bypass -File switch\slippi\tests\run-tests.ps1 [-Clang path\to\clang++.exe]
param([string]$Clang = "E:\tools\llvm-mingw\llvm-mingw-20261006-ucrt-x86_64\bin\clang++.exe")
$ErrorActionPreference = "Stop"
$S = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent $S)
$out = Join-Path $repo "build\slippi-tests"
New-Item -ItemType Directory -Force (Join-Path $out "obj") | Out-Null
$cc = Join-Path (Split-Path -Parent $Clang) "clang.exe"
$inc = @("-I$S", "-I$S\third_party\enet\include", "-I$repo\launcher\include")

$exe = Join-Path $out "slippi-protocol-tests.exe"
& $Clang -std=c++20 -O1 -Wall @inc (Join-Path $S "slippi_protocol.cpp") (Join-Path $S "tests\protocol_tests.cpp") -o $exe -static
if ($LASTEXITCODE) { throw "building the protocol tests failed" }
& $exe
if ($LASTEXITCODE) { throw "protocol tests failed" }

$enetObjs = @()
foreach ($c in Get-ChildItem (Join-Path $S "third_party\enet") -Filter *.c | Where-Object { $_.Name -ne "unix.c" }) {
    $o = Join-Path $out ("obj\" + $c.BaseName + ".o")
    & $cc -std=gnu11 -O1 -w @inc -c $c.FullName -o $o
    if ($LASTEXITCODE) { throw "compiling $($c.Name) failed" }
    $enetObjs += $o
}
$exe = Join-Path $out "slippi-net-tests.exe"
& $Clang -std=c++20 -O1 -Wall @inc (Join-Path $S "slippi_protocol.cpp") (Join-Path $S "slippi_net.cpp") `
    (Join-Path $S "tests\net_tests.cpp") @enetObjs -o $exe -static -lws2_32 -lwinmm
if ($LASTEXITCODE) { throw "building the network tests failed" }
& $exe
if ($LASTEXITCODE) { throw "network tests failed" }

# The host command handler against a scripted game side and a scripted remote player.
$hostDir = Join-Path $out "host"
New-Item -ItemType Directory -Force $hostDir | Out-Null
$exe = Join-Path $out "slippi-host-tests.exe"
& $Clang -std=c++20 -O1 -Wall @inc "-I$repo\switch\src" (Join-Path $S "slippi_protocol.cpp") (Join-Path $S "slippi_net.cpp") `
    (Join-Path $S "slippi_host.cpp") (Join-Path $S "tests\host_tests.cpp") @enetObjs -o $exe -static -lws2_32 -lwinmm
if ($LASTEXITCODE) { throw "building the host tests failed" }
& $exe $hostDir
if ($LASTEXITCODE) { throw "host tests failed" }
