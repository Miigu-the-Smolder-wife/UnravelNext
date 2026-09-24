param(
  [string]$Track = "core",     # core | V | M | S | R | C | ... : each session builds in its own folder build/<Track>
  [string]$Config = "Release",
  [string]$Target = ""         # optional single target, e.g. unx_unit_tests
)
# Configure (CMake + Ninja, VS 18 MSVC) and build: native libraries, the shader compiler, every kernel (DXIL size limit
# enforced), every auto-registered module, tool, test and gate. Incremental builds only redo what changed.
# Parallel sessions never share a build folder (INTERFACES_KO.md 3.1).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$buildDir = Join-Path $root "build\$Track"
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
foreach ($sub in @("External/nvapi", "External/flip")) {
  if ((Test-Path (Join-Path $root ".gitmodules")) -and -not (Test-Path (Join-Path $root "$sub\.git"))) {
    & git -C $root submodule update --init $sub
    if ($LASTEXITCODE -ne 0) { throw "submodule init failed: $sub" }
  }
}
New-Item -ItemType Directory -Force $buildDir | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew()
$targetArg = if ($Target) { "--target $Target" } else { "" }
$cmd = "`"$vcvars`" >nul 2>&1 && `"$cmake`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config -S `"$root`" -B `"$buildDir`" >nul && `"$cmake`" --build `"$buildDir`" $targetArg"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
"build ok in {0:N1} s -> {1}" -f $sw.Elapsed.TotalSeconds, $buildDir
