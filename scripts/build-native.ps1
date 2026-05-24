param(
    [string]$Ndk = "D:\sc\android-ndk-r27d-windows\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$ClangBin = Join-Path $Ndk "toolchains\llvm\prebuilt\windows-x86_64\bin"
$OutBin = Join-Path $Root "module\bin"
$OutZygisk = Join-Path $Root "module\zygisk"
$ZygiskInclude = Join-Path $Root "third_party\zygisk"

New-Item -ItemType Directory -Force $OutBin, $OutZygisk | Out-Null

function Build-Arch {
    param(
        [string]$Name,
        [string]$Compiler,
        [string]$Strip
    )

    $Cxx = Join-Path $ClangBin $Compiler
    $StripExe = Join-Path $ClangBin $Strip
    if (-not (Test-Path $Cxx)) {
        throw "Missing compiler: $Cxx"
    }

    $BuildDir = Join-Path $Root "build\$Name"
    New-Item -ItemType Directory -Force $BuildDir | Out-Null

    $common = @("-std=c++17", "-fPIE", "-Wall", "-Wextra", "-Os")
    & $Cxx @common "$Root\src\native\scene_netnsctl.cpp" "-o" "$BuildDir\scene-netnsctl" "-static-libstdc++"
    & $Cxx @common "$Root\src\native\su_wrapper.cpp" "-o" "$BuildDir\su" "-static-libstdc++"

    $soArgs = @(
        "-std=c++17", "-fPIC", "-shared", "-Wall", "-Wextra", "-Os",
        "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit",
        "-I$ZygiskInclude",
        "$Root\src\zygisk\scene_netns_zygisk.cpp",
        "-llog",
        "-nostdlib++",
        "-o", "$BuildDir\arm.so"
    )
    & $Cxx @soArgs

    if (Test-Path $StripExe) {
        & $StripExe "$BuildDir\scene-netnsctl", "$BuildDir\su", "$BuildDir\arm.so"
    }

    if ($Name -eq "arm64-v8a") {
        Copy-Item "$BuildDir\scene-netnsctl" "$OutBin\scene-netnsctl" -Force
        Copy-Item "$BuildDir\su" "$OutBin\su" -Force
    }
    Copy-Item "$BuildDir\arm.so" "$OutZygisk\$Name.so" -Force
}

Build-Arch -Name "arm64-v8a" -Compiler "aarch64-linux-android23-clang++.cmd" -Strip "llvm-strip.exe"
Build-Arch -Name "armeabi-v7a" -Compiler "armv7a-linux-androideabi23-clang++.cmd" -Strip "llvm-strip.exe"

Write-Host "Native artifacts written to $OutBin and $OutZygisk"
