param(
  [string]$Config = "Release"
)
# Configures and builds the standalone design benchmarks (Bench/CMakeLists.txt) into Tools/DesignBench/build
# (VS 18 MSVC + Ninja + CMake, the same toolchain as Tools/Microbench/Build.ps1).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "Bench"
$build = Join-Path $root "build"
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
New-Item -ItemType Directory -Force $build | Out-Null
$cmd = "`"$vcvars`" >nul 2>&1 && `"$cmake`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config -S `"$src`" -B `"$build`" && `"$cmake`" --build `"$build`" --config $Config"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
