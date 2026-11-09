# Builds the Melee NX launcher NRO on Windows with a local devkitPro (no Docker).
#
#   powershell -ExecutionPolicy Bypass -File launcher\build-native.ps1 [-DevkitPro E:\tools\devkitPro] [-Out DIR]
#
# Output: <Out>\melee-nx-launcher.nro (default Out: build\launcher). Same sources and libraries as
# launcher/CMakeLists.txt, which is the Docker/Linux path.
param([string]$DevkitPro = $(if ($env:DEVKITPRO -and (Test-Path $env:DEVKITPRO)) { $env:DEVKITPRO } else { "E:\tools\devkitPro" }),
      [string]$Out = "")
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$L = Join-Path $repo "launcher"
if (-not $Out) { $Out = Join-Path $repo "build\launcher" }
$gxx = Join-Path $DevkitPro "devkitA64\bin\aarch64-none-elf-g++.exe"
$gcc = Join-Path $DevkitPro "devkitA64\bin\aarch64-none-elf-gcc.exe"
if (-not (Test-Path $gxx)) { throw "devkitA64 not found at $DevkitPro (pass -DevkitPro)" }
$obj = Join-Path $Out "obj"
New-Item -ItemType Directory -Force $obj | Out-Null

$cmake = Get-Content (Join-Path $L "CMakeLists.txt") -Raw
function CMakeVar($name) { if ($cmake -match "set\($name `"([^`"]*)`"\)") { $Matches[1] } else { throw "no $name in CMakeLists.txt" } }
$version = CMakeVar "LAUNCHER_VERSION"

$srcs = @(Get-ChildItem (Join-Path $L "source") -Recurse -Include *.cpp, *.c | ForEach-Object FullName) +
        @((Join-Path $repo "switch\src\game_data\disc_extractor.cpp"), (Join-Path $repo "switch\src\nod\gc_disc.c"))
$arch = @("-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE")
$cflags = $arch + @("-O2", "-DNDEBUG", "-D__SWITCH__=1", "-DCURL_STATICLIB=1", "-DMNX_LAUNCHER_VERSION_TOKEN=$version",
    "-ffunction-sections", "-fdata-sections", "-Wall", "-Wno-unused-parameter",
    "-I$L\include", "-I$L\source", "-I$repo\switch\src\game_data",
    "-I$DevkitPro\libnx\include", "-I$DevkitPro\portlibs\switch\include", "-I$DevkitPro\portlibs\switch\include\SDL2",
    "-I$DevkitPro\portlibs\switch\include\freetype2", "-I$DevkitPro\portlibs\switch\include\harfbuzz")
$env:DEVKITPRO = $DevkitPro

$objs = @()
$i = 0
$failed = @()
foreach ($s in $srcs) {
    $o = Join-Path $obj ("{0:d3}-{1}.o" -f $i++, [IO.Path]::GetFileNameWithoutExtension($s))
    if ($s.EndsWith(".c")) { & $gcc "-std=gnu17" @cflags -c $s -o $o } else { & $gxx "-std=gnu++20" @cflags -c $s -o $o }
    if ($LASTEXITCODE) { $failed += $s }
    $objs += $o
}
if ($failed.Count) { throw "compiling failed: $($failed -join ', ')" }

$elf = Join-Path $Out "melee-nx-launcher.elf"
$link = @("-specs=$DevkitPro\libnx\switch.specs") + $arch + @("-Wl,--gc-sections") + $objs + @(
    "-L$DevkitPro\libnx\lib", "-L$DevkitPro\portlibs\switch\lib", "-Wl,--start-group",
    "-lSDL2_ttf", "-lSDL2_image", "-lSDL2", "-lharfbuzz", "-lfreetype", "-lbz2", "-lpng16", "-ljpeg", "-lwebp", "-lwebpdemux",
    "-lcurl", "-lmbedtls", "-lmbedx509", "-lmbedcrypto", "-lzstd", "-lz",
    "-lEGL", "-lglapi", "-ldrm_nouveau", "-lstdc++", "-lnx", "-lm", "-lpthread", "-Wl,--end-group", "-o", $elf)
& $gxx @link
if ($LASTEXITCODE) { throw "linking the launcher failed" }

$nacp = Join-Path $Out "melee-nx-launcher.nacp"
& (Join-Path $DevkitPro "tools\bin\nacptool.exe") --create (CMakeVar "LAUNCHER_TITLE") (CMakeVar "LAUNCHER_AUTHOR") $version $nacp
if ($LASTEXITCODE) { throw "nacptool failed" }
$icon = Join-Path $repo "switch\icon.jpg"  # same icon as the game
$nro = Join-Path $Out "melee-nx-launcher.nro"
& (Join-Path $DevkitPro "tools\bin\elf2nro.exe") $elf $nro "--nacp=$nacp" "--icon=$icon" "--romfsdir=$L\romfs"
if ($LASTEXITCODE) { throw "elf2nro failed" }
$hash = (Get-FileHash $nro -Algorithm SHA256).Hash.ToLower()
Write-Host "launcher: $nro ($([math]::Round((Get-Item $nro).Length / 1MB, 1)) MB) sha256 $hash"
