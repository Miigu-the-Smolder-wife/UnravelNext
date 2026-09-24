param(
  [string]$Config = "Release",
  [switch]$Run,
  [string[]]$RunArgs = @()
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
New-Item -ItemType Directory -Force $build | Out-Null
$cmd = "`"$vcvars`" >nul 2>&1 && `"$cmake`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config -S `"$root`" -B `"$build`" && `"$cmake`" --build `"$build`" --config $Config"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
if ($Run) {
  & (Join-Path $build "Microbench.exe") @RunArgs
}
