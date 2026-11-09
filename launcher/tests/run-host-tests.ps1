# Builds and runs the launcher host tests with a Windows clang (llvm-mingw).
#   powershell -ExecutionPolicy Bypass -File launcher\tests\run-host-tests.ps1 [-Clang path\to\clang++.exe]
param([string]$Clang = "E:\tools\llvm-mingw\llvm-mingw-20261006-ucrt-x86_64\bin\clang++.exe")
$ErrorActionPreference = "Stop"
$L = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent $L
$out = Join-Path $repo "build\launcher-tests"
New-Item -ItemType Directory -Force $out | Out-Null
$srcs = @("tests\launcher_tests.cpp", "tests\stub\host_stubs.cpp", "source\data\dat_identity.cpp", "source\data\mods.cpp",
          "source\data\settings.cpp", "source\data\slippi_account.cpp", "source\data\lan_signin.cpp",
          "source\data\launch.cpp") | ForEach-Object { Join-Path $L $_ }
$srcs += Join-Path $repo "switch\src\game_data\launch_request.cpp"
$exe = Join-Path $out "launcher-tests.exe"
& $Clang -std=c++20 -O1 -Wall -Wno-unused-parameter -DMNX_NO_SOCKETS "-I$L\tests\stub" "-I$L\include" "-I$L\source" `
    "-I$repo\switch\src\game_data" @srcs -o $exe -static
if ($LASTEXITCODE) { throw "building the host tests failed" }
& $exe
if ($LASTEXITCODE) { throw "host tests failed" }
