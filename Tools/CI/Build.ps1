param(
  [string]$Config = "Release",
  [string]$BuildDir = ""
)
# Configure (CMake + Ninja, VS 18 MSVC) and build everything: native libraries, the shader compiler, every kernel
# (DXIL size limit enforced), tests and gates. Incremental builds only redo what changed (Ninja + DXC depfiles).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
if (-not $BuildDir) { $BuildDir = Join-Path $root "build\$Config" }
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path (Join-Path $root "External\nvapi\nvapi.h"))) { & git -C $root submodule update --init External/nvapi; if ($LASTEXITCODE -ne 0) { throw "submodule init failed" } }
New-Item -ItemType Directory -Force $BuildDir | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew()
$cmd = "`"$vcvars`" >nul 2>&1 && `"$cmake`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config -S `"$root`" -B `"$BuildDir`" >nul && `"$cmake`" --build `"$BuildDir`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
"build ok in {0:N1} s -> {1}" -f $sw.Elapsed.TotalSeconds, $BuildDir
