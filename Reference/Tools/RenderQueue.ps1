param(
  [string]$Track = "C",
  [string]$Only = ""     # optional substring filter on "<scene>/<camera>/<res>"
)
# Renders the gate references into Cache/Reference (INTERFACES_KO.md 10.2) one after another, CPU only, at
# below-normal priority so builds and CPU measurements of other sessions preempt it. Each render checkpoints every
# 10 minutes and resumes after an interruption; finished references are reused (the tool skips cached ones).
# Every render pauses (within ~0.3 s, no CPU used) while another session holds the GPU lock for a timing run
# (.gpulock/current.json with a live holder and "kind": "timing" or no kind; correctness runs do not pause it) or while the manual marker .gpulock/HOLD exists — create HOLD to stop the
# queue's CPU use (e.g. while the user plays a game), delete it to resume.
# Cache/Reference/PAUSE_QUEUE pauses only the queue (for a one-off render that must not run beside it). Each render uses
# 3/4 of the logical processors (unx_reference default) so other sessions' builds and tests keep their share. Setup (BVH, atmosphere table) waits too.
# Scenes with wind render with --no-wind: the reference does not trace 1.1 M wind-deformed instances (the engine's
# quality comparison then uses the same wind-free scene, written next to the reference with --write-scene).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$log = Join-Path $root "Cache\Reference\queue.log"
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
# Run from a snapshot of the binaries so the session can keep rebuilding build/<Track> while references render for
# hours (a running executable and its DLLs are never overwritten).
$bin = Join-Path $root "Cache\Reference\bin"
if (Get-Process unx_reference -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$bin*" }) { throw "a queue is already running from $bin" }
New-Item -ItemType Directory -Force $bin | Out-Null
foreach ($f in @("unx_reference.exe", "embree4.dll", "tbb12.dll", "tbbmalloc.dll")) { Copy-Item (Join-Path $root "build\$Track\bin\$f") $bin -Force }
$exe = Join-Path $bin "unx_reference.exe"
# Queue log lines: a reader holding the file (e.g. a tail) must not stop the queue, so writes retry briefly and then
# give up on that line only.
function Write-QueueLog([string]$line) {
  for ($i = 0; $i -lt 20; $i++) {
    try { Add-Content -Path $log -Value $line -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 250 }
  }
}
$jobs = @(
  @{ scene = "ridge_sunset"; camera = "ridge";   res = "2560x1440"; wind = $false },
  @{ scene = "ridge_sunset"; camera = "ridge";   res = "3840x2160"; wind = $false },
  @{ scene = "city_block";  camera = "street";   res = "2560x1440"; wind = $true },
  @{ scene = "forest_combat"; camera = "eye";    res = "2560x1440"; wind = $true },
  @{ scene = "forest_combat"; camera = "up";     res = "2560x1440"; wind = $true },
  @{ scene = "forest_combat"; camera = "edge";   res = "2560x1440"; wind = $true },
  @{ scene = "forest_combat"; camera = "vista";  res = "2560x1440"; wind = $true },
  @{ scene = "forest_thin"; camera = "forest";   res = "2560x1440"; wind = $true },
  @{ scene = "forest_card"; camera = "forest";   res = "2560x1440"; wind = $true },
  @{ scene = "city_block";  camera = "street";   res = "3840x2160"; wind = $true },
  @{ scene = "forest_thin"; camera = "forest";   res = "3840x2160"; wind = $true },
  @{ scene = "interior";    camera = "floor_60"; res = "2560x1440"; wind = $false },
  @{ scene = "interior";    camera = "mirror";   res = "2560x1440"; wind = $false },
  @{ scene = "city_night";  camera = "wet_road"; res = "2560x1440"; wind = $true },
  @{ scene = "waterside";   camera = "lake";     res = "2560x1440"; wind = $true },
  @{ scene = "forest_combat"; camera = "eye";    res = "3840x2160"; wind = $true },
  @{ scene = "forest_combat"; camera = "up";     res = "3840x2160"; wind = $true },
  @{ scene = "forest_combat"; camera = "edge";   res = "3840x2160"; wind = $true },
  @{ scene = "forest_combat"; camera = "vista";  res = "3840x2160"; wind = $true }
)
foreach ($j in $jobs) {
  $key = "$($j.scene)/$($j.camera)/$($j.res)"
  if ($Only -and -not $key.Contains($Only)) { continue }
  $args2 = @("render", "--scene", $j.scene, "--camera", $j.camera, "--res", $j.res, "--also-hold", (Join-Path $root "Cache\Reference\PAUSE_QUEUE"))
  if ($j.wind) {
    $args2 += "--no-wind"
    $args2 += @("--write-scene", (Join-Path $root "Cache\Scenes\$($j.scene)_nowind.unxscene"))
  }
  Write-QueueLog ("[{0}] start {1}" -f (Get-Date -Format s), $key)
  $p = Start-Process -FilePath $exe -ArgumentList $args2 -NoNewWindow -PassThru -RedirectStandardError "$log.$($j.scene).$($j.camera).$($j.res).txt"
  $p.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal
  $p.WaitForExit()
  Write-QueueLog ("[{0}] end {1} exit {2}" -f (Get-Date -Format s), $key, $p.ExitCode)
}
