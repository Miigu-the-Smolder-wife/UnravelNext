# [CmdletBinding()]: an unknown parameter is an error instead of an ignored argument (S passed -BuildDir, which a plain
# param block dropped into $args, and the build went on with the default -Track core in another session's folder).
[CmdletBinding()]
param(
  [string]$Track = "core",     # core | M | S | R | C | I | FX | RPP | all : each session builds in its own folder build/<Track>
  [string]$Tracks = "",        # enabled tracks (cmake/Tracks.cmake); default from -Track: core -> V, M -> M, S -> S, R -> R,
                               # C -> C, V -> V, I -> V;M;S;R;I (the host links the whole renderer), RPP -> C;RPP (the
                               # RPP-1 scene build links SceneGen),
                               # all -> all (integrated build for gate measurements)
  [string]$Config = "Release",
  [string]$Target = "",        # optional single target, e.g. unx_unit_tests
  [switch]$Committed,          # build a commit, not the shared working tree: a git worktree next to the repository
                               # (..\UnravelNext-gate) is checked out at -Ref and built there (INTERFACES_KO.md 3.5)
  [string]$Ref = "HEAD",       # commit for -Committed
  [int]$Jobs = 0,              # build parallelism (0 = Ninja's default: every core); e.g. -Jobs 4 while the user plays
  [switch]$LowPriority         # configure and build at BelowNormal priority (the compilers inherit it)
)
# Configure (CMake + Ninja, VS 18 MSVC) and build: native libraries, the shader compiler, every kernel (DXIL size limit
# enforced), every auto-registered module, tool, test and gate of the enabled tracks. Disabled tracks' entry points are
# core's empty stubs, so one session's unfinished files never break another session's build. Incremental builds only
# redo what changed. Parallel sessions never share a build folder (INTERFACES_KO.md 3.1).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
# git writes progress and notes ("Previous HEAD position ...") to stderr; run it with stderr as text and judge by exit
# code, so calling this script from PowerShell (& .\Build.ps1) under "Stop" does not end on a note.
function Invoke-Git {
  $saved = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try { $out = & git @args 2>&1 | ForEach-Object { "$_" } } finally { $ErrorActionPreference = $saved }
  $out | Where-Object { $_ } | ForEach-Object { Write-Host $_ }
}
function Get-GitOutput {
  $saved = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try { return (& git @args 2>$null) } finally { $ErrorActionPreference = $saved }
}
$submodules = @("External/nvapi", "External/flip", "External/meshoptimizer")

if ($Committed) {
  # Integrated gate builds compile only committed code (INTERFACES_KO.md 3.5): other sessions' uncommitted files in the
  # shared tree never enter them, and the results record build.commit = that commit with build.dirty = false. The
  # worktree shares the repository's objects but not its index, so staging and commits elsewhere are unaffected. One
  # -Committed build at a time (a named mutex): a second waits.
  $gate = Join-Path (Split-Path -Parent $root) "UnravelNext-gate"
  $mutex = New-Object System.Threading.Mutex($false, "Global\UnravelNextCommittedBuild")
  [void]$mutex.WaitOne()
  try {
    $sha = Get-GitOutput -C $root rev-parse --verify "$Ref^{commit}"
    if ($LASTEXITCODE -ne 0 -or -not $sha) { throw "unknown commit: $Ref" }
    $sha = $sha.Trim()
    if (-not (Test-Path (Join-Path $gate ".git"))) {
      Invoke-Git -C $root worktree add --detach $gate $sha
      if ($LASTEXITCODE -ne 0) { throw "git worktree add failed" }
    } else {
      Invoke-Git -C $gate checkout --detach --force $sha
      if ($LASTEXITCODE -ne 0) { throw "checkout $sha in $gate failed" }
      Invoke-Git -C $gate clean -fdq  # untracked files that are not ignored; build\ and External\.cache stay
    }
    # Submodules from the main checkout's objects (no network); the dependency cache is copied once.
    foreach ($sub in $submodules) {
      Invoke-Git -C $gate submodule update --init --reference (Join-Path $root $sub) -- $sub
      if ($LASTEXITCODE -ne 0) { throw "submodule $sub in $gate failed" }
    }
    $cache = Join-Path $root "External\.cache"
    if ((Test-Path $cache) -and -not (Test-Path (Join-Path $gate "External\.cache"))) { Copy-Item -Recurse $cache (Join-Path $gate "External\.cache") }
    "committed build of $sha in $gate"
    $childArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $gate "Tools\CI\Build.ps1"), "-Track", $Track, "-Config", $Config)
    if ($Tracks) { $childArgs += @("-Tracks", $Tracks) }
    if ($Target) { $childArgs += @("-Target", $Target) }
    if ($Jobs -gt 0) { $childArgs += @("-Jobs", $Jobs) }
    if ($LowPriority) { $childArgs += "-LowPriority" }
    & powershell @childArgs
    if ($LASTEXITCODE -ne 0) { throw "committed build failed ($LASTEXITCODE)" }
  } finally {
    $mutex.ReleaseMutex()
  }
  return
}

$buildDir = Join-Path $root "build\$Track"
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
foreach ($sub in $submodules) {
  if ((Test-Path (Join-Path $root ".gitmodules")) -and -not (Test-Path (Join-Path $root "$sub\.git"))) {
    Invoke-Git -C $root submodule update --init $sub
    if ($LASTEXITCODE -ne 0) { throw "submodule init failed: $sub" }
  }
}
if (-not $Tracks) {
  $Tracks = switch ($Track) { "core" { "V" } "I" { "V;M;S;R;I" } "RPP" { "C;RPP" } "E" { "FX;E" } "all" { "all" } default { $Track } }
}
# A build folder is configured for one track set: Ninja's dyndep state from another set can abort the build
# (edge->outputs_ready assertion, reported by I). A different set starts the folder over.
$cacheFile = Join-Path $buildDir "CMakeCache.txt"
if (Test-Path $cacheFile) {
  $line = Select-String -Path $cacheFile -Pattern "^UNX_TRACKS:STRING=(.*)$" | Select-Object -First 1
  $previous = if ($line) { $line.Matches[0].Groups[1].Value } else { "" }
  if ($previous -ne $Tracks) {
    Write-Warning "build\$Track was configured for tracks '$previous', now '$Tracks': rebuilding the folder from scratch"
    try { Remove-Item -Recurse -Force $buildDir } catch { throw "cannot clear $buildDir (a program from it is running?): $_" }
  }
}
New-Item -ItemType Directory -Force $buildDir | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew()
$targetArg = if ($Target) { "--target $Target" } else { "" }
if ($Jobs -gt 0) { $targetArg += " -j $Jobs" }
if ($LowPriority) { (Get-Process -Id $PID).PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal }
$cmd = "`"$vcvars`" >nul 2>&1 && `"$cmake`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config -DUNX_TRACKS=`"$Tracks`" -S `"$root`" -B `"$buildDir`" >nul && `"$cmake`" --build `"$buildDir`" $targetArg"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
# Header dependencies (v1.36): Ninja records a compile's headers from cl's /showIncludes lines, whose prefix is the
# localised text CMake detected at configure time. A build run under another console code page (cmake --build from a
# UTF-8 shell against a folder configured from CP949) records none, and a header-only change then rebuilds nothing.
# Configure and build always run in the one cmd above; this check fails loudly if any object that includes a project
# header ("...") has no recorded header, so a stale binary is never measured.
$missing = @()
foreach ($line in (& $ninja -C $buildDir -t deps 2>$null)) {
  if ($line -notmatch '^(\S+\.obj): #deps 0,') { continue }
  $obj = $Matches[1]
  if ($obj -notmatch '^(.*?)CMakeFiles/[^/]+\.dir/(.+)\.obj$') { continue }
  $source = Join-Path $root (($Matches[1] -replace '^\./', '') + $Matches[2])
  if ((Test-Path $source) -and (Select-String -Path $source -Pattern '^\s*#\s*include\s+"' -Quiet)) { $missing += $obj }
}
if ($missing.Count -gt 0) {
  throw ("{0} objects in {1} include project headers but Ninja recorded none (built outside Build.ps1 under another code page?): {2}. " +
         "Rebuild them through Build.ps1 after deleting those .obj files." -f $missing.Count, $buildDir, ($missing -join ", "))
}
"build ok in {0:N1} s -> {1} (tracks: core;{2})" -f $sw.Elapsed.TotalSeconds, $buildDir, $Tracks
