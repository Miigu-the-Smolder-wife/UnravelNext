# Local verification of a cloud branch (cloud sessions have no GPU): build, tests, native-vs-upscaled captures,
# determinism pairs, a luminance log and timings, then Tools/Verify/verify_analyze.py writes SUMMARY_KO.md.
# Runbook: Docs/Status/LOCAL_VERIFY_RUNBOOK_KO.md.
#   powershell -NoProfile -ExecutionPolicy Bypass -File Tools\Verify\Verify-CloudBranch.ps1
#   powershell ... -File Tools\Verify\Verify-CloudBranch.ps1 -Phases tests,caps        (only some phases)
#   powershell ... -File Tools\Verify\Verify-CloudBranch.ps1 -Quick                    (a short smoke run of every phase)
#   powershell ... -File Tools\Verify\Verify-CloudBranch.ps1 -Phases publish              (evidence to origin/local/verify-<sha>)
# Safety: every GPU run happens under Tools/CI/GpuLock.ps1; the script stops when a game runs or a device is removed;
# it never kills processes and never edits engine code (the gate worktree is checked out by Build.ps1 -Committed).
[CmdletBinding()]
param(
  [string]$Ref = "origin/cloud/render-fixes",
  [string]$Out = "",
  [string[]]$Phases = @("build", "tests", "caps", "convergence", "motion", "relight", "cut", "determinism", "luminance", "timing", "report"),
  [string]$GameProject = "C:\Users\USER\UnravelGames\BathhouseTycoon",          # -Phases unity (after a deployment)
  [string]$GameScene = "Assets/Game/BathhouseTycoon/Scenes/Bathhouse.unity",
  [switch]$Quick,
  [switch]$KeepPfm, # keep the PFM captures after the report (default: deleted - motion, relight and cut make tens of GB per run)
  [switch]$DropPfm, # (the default now; accepted for older callers)
  [switch]$Local,   # build and run in this checkout (build\all of its working tree) instead of ..\UnravelNext-gate at -Ref
                    # (an implementation session measuring its own branch; the coordination session keeps the default)
  [switch]$DryRun   # write each GPU batch and parse it, run nothing on the GPU (checks the script itself)
)
$ErrorActionPreference = "Continue"  # (native tools write to stderr; failures are checked by exit code and thrown explicitly)
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$gate = if ($Local) { $root } else { Join-Path (Split-Path -Parent $root) "UnravelNext-gate" }
$games = @("League of Legends", "Remnant2-Win64-Shipping", "Overwatch", "VALORANT-Win64-Shipping", "cs2", "r5apex", "FortniteClient-Win64-Shipping")
if ($Phases.Count -eq 1 -and $Phases[0] -match ",") { $Phases = $Phases[0] -split "," }

function Test-Game {
  $g = Get-Process -Name $games -ErrorAction SilentlyContinue
  if ($g) { throw "게임 실행 중($($g[0].Name)): GPU 작업을 하지 않는다. 게임을 끝낸 뒤 다시 실행한다." }
}
$gpuPhases = @($Phases | Where-Object { $_ -in @("build", "tests", "caps", "convergence", "motion", "relight", "cut", "determinism", "luminance", "timing") }).Count -gt 0
if ($gpuPhases -and -not $DryRun) { Test-Game }
& git -C $root fetch -q origin 2>$null
$sha = if ($Local) { (& git -C $root rev-parse --short HEAD).Trim() + $(if (& git -C $root status --porcelain --untracked-files=no) { "-dirty" } else { "" }) } else { (& git -C $root rev-parse --short $Ref).Trim() }
if (-not $Out) { $Out = Join-Path $root "Results\Local\Verify-$sha" }
foreach ($d in "", "tests", "caps", "det", "lum", "timing", "motion", "relight", "cut") { New-Item -ItemType Directory -Force (Join-Path $Out $d) | Out-Null }
$steps = Join-Path $Out "steps.csv"
$exe = Join-Path $gate "build\all\bin\unx_gate_shadow_renderergate.exe"
$scenes = @{ bath = "Results\R\GiInterior\bath_bt0_ev6.unxscene"; train = "Results\R\GiInterior\train_v0_ev6.unxscene" }
foreach ($s in $scenes.Values) { if (-not (Test-Path (Join-Path $root $s))) { throw "장면 파일이 없다: $root\$s" } }
"ref $Ref = $sha, out $Out, phases $($Phases -join ','), quick $Quick"

# One batch of GPU runs inside one GPU lock: writes a child script with the runs and executes it through GpuLock.
function Invoke-Locked([string]$kind, [string[]]$lines) {
  if (-not $DryRun) { Test-Game }
  $child = Join-Path $Out "batch_$([guid]::NewGuid().ToString('N').Substring(0, 8)).ps1"
  $head = @(
    '$ErrorActionPreference = "Continue"',
    "Set-Location '$gate'",
    ("`$games = @('" + ($games -join "','") + "')"),
    'function Invoke-Step($name, $file, [string[]]$a) {',
    '  if (Get-Process -Name $games -ErrorAction SilentlyContinue) { "STOP game"; exit 3 }',
    '  $sw = [Diagnostics.Stopwatch]::StartNew()',
    "  `$log = Join-Path '$Out' (`$name + '.log')",
    '  & $file @a *> $log',
    '  $code = $LASTEXITCODE',
    '  $hung = [bool](Select-String -Path $log -Pattern "DEVICE_(HUNG|REMOVED|RESET)|887A0005|887A0006" | Where-Object { $_.Line -notmatch "test\.(child|throw)" })',
    '  $dbg = (Select-String -Path $log -Pattern "\[d3d12 (ERROR|CORRUPTION|WARNING)").Count',
    "  Add-Content -Encoding utf8 '$steps' (`"`$name,`$code,`$dbg,`$([int]`$sw.Elapsed.TotalSeconds)`")",
    '  "$name exit $code d3d12 $dbg"',
    '  if ($hung) { "STOP device removed in $name"; exit 87 }',
    '}')
  Set-Content -Encoding utf8 $child ($head + $lines)
  if ($DryRun) { $e = $null; $k = $null; $null = [System.Management.Automation.Language.Parser]::ParseFile($child, [ref]$k, [ref]$e); "dry run: $kind batch, $($lines.Count) runs, parse errors $($e.Count) ($child)"; return }
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "Tools\CI\GpuLock.ps1") -Track all -Kind $kind -TimeoutMinutes 90 -- powershell -NoProfile -ExecutionPolicy Bypass -File $child
  $code = $LASTEXITCODE
  Remove-Item $child
  if ($code -eq 87) { throw "장치 제거(TDR): 모든 GPU 실행을 멈추고 조정 세션에 알린다. 로그: $Out" }
  if ($code -eq 3) { throw "게임이 시작되어 멈췄다. 게임이 끝난 뒤 남은 단계만 -Phases로 다시 실행한다." }
}
function Q([string]$s) { "'" + $s.Replace("'", "''") + "'" }

if ($Phases -contains "build" -and -not $DryRun) {
  "== build $Ref"
  $log = Join-Path $Out "build.log"
  if ($Local) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "Tools\CI\Build.ps1") -Track all *> $log }
  else { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "Tools\CI\Build.ps1") -Track all -Committed -Ref $Ref *> $log }
  if ($LASTEXITCODE -ne 0 -and $Local) { throw "빌드 실패: $log" }
  if ($LASTEXITCODE -ne 0) {
    # A build stopped earlier can leave 0-byte object files (LNK1136); remove them once and build again.
    $empty = Get-ChildItem (Join-Path $gate "build\all") -Recurse -Include *.obj -ErrorAction SilentlyContinue | Where-Object { $_.Length -eq 0 }
    if ($empty) { $empty | Remove-Item; & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "Tools\CI\Build.ps1") -Track all -Committed -Ref $Ref *> $log }
    if ($LASTEXITCODE -ne 0) { throw "빌드 실패: $log (FAILED 줄과 error 줄을 클라우드 세션에 전달한다)" }
  }
  (& git -C $gate rev-parse --short HEAD).Trim() | Set-Content (Join-Path $Out "gate_sha.txt")
}
# The committed build leaves the gate worktree clean, which removes the scene links: link them again every time.
foreach ($s in $(if ($Local) { @() } else { $scenes.Values })) {
  $dst = Join-Path $gate $s
  New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
  if (-not (Test-Path $dst)) { New-Item -ItemType HardLink -Path $dst -Target (Join-Path $root $s) | Out-Null }
}
if ($gpuPhases -and -not (Test-Path $exe)) { throw "renderergate가 없다: 먼저 -Phases build" }

$frames = if ($Quick) { @("--frames", "60", "--warmup-frames", "60") } else { @() }
$resolutions = if ($Quick) { @("1920x1080") } else { @("2560x1440", "1920x1080") }
$sceneNames = if ($Quick) { @("train") } else { @("train", "bath") }

if ($Phases -contains "tests") {
  "== tests"
  $tests = @("unx_test_reflection_reflectionanalytic", "unx_test_gi_gianalytic", "unx_test_host_hostmotion", "unx_test_shadow_localshadowtests", "unx_test_shadow_froxeltests")
  if ($Quick) { $tests = @("unx_test_shadow_localshadowtests") }
  Invoke-Locked "correctness" ($tests | ForEach-Object { "Invoke-Step $(Q "tests\$_") $(Q (Join-Path $gate "build\all\bin\$_.exe")) @()" })
}
if ($Phases -contains "caps") {
  "== captures (native vs upscaled)"
  $lines = @()
  foreach ($sc in $sceneNames) {
    foreach ($r in $resolutions) {
      $modes = @(@{ m = "static"; a = @() })
      # "moving": a 90 deg/s turn (the scenes have no camera path, so --moving kept the camera still before 2026-10-01)
      if ($sc -eq "train") { $modes += @{ m = "moving"; a = @("--path-rotate", "90") }; $modes += @{ m = "sun"; a = @("--sun-deg-per-s", "20") } }
      foreach ($md in $modes) {
        $base = "$($sc)_$($r)_$($md.m)"
        $common = @("--scene", $scenes[$sc], "--resolution", $r) + $md.a + $frames
        $lines += "Invoke-Step $(Q "caps\$($base)_native") $(Q $exe) @(" + ((($common + @("--capture", (Join-Path $Out "caps\$($base)_native.pfm"))) | ForEach-Object { Q $_ }) -join ",") + ")"
        $lines += "Invoke-Step $(Q "caps\$($base)_up") $(Q $exe) @(" + ((($common + @("--capture-output", (Join-Path $Out "caps\$($base)_up.pfm"))) | ForEach-Object { Q $_ }) -join ",") + ")"
      }
    }
  }
  Invoke-Locked "correctness" $lines
}
if ($Phases -contains "convergence") {
  # Cold start (the worst disocclusion; a light edit rebuild is the same today): the upscaled output after k frames
  # against the output after 600 frames (user requirement 2026-09-30: clean within a few frames, never seconds).
  "== convergence (cold start: frames 1, 4, 16, 64, 256 vs 600)"
  $lines = @()
  $ks = if ($Quick) { @(1, 4) } else { @(1, 4, 16, 64, 256) }
  $refFrames = if ($Quick) { "60" } else { "600" }
  foreach ($sc in $sceneNames) {
    foreach ($r in $resolutions) {
      foreach ($k in ($ks + @("ref"))) {
        $n = if ($k -eq "ref") { $refFrames } else { "$k" }
        $a = @("--scene", $scenes[$sc], "--resolution", $r, "--warmup-frames", "0", "--frames", $n, "--capture-output", (Join-Path $Out "conv\$($sc)_$($r)_$k.pfm"))
        $lines += "Invoke-Step $(Q "conv\$($sc)_$($r)_$k") $(Q $exe) @(" + (($a | ForEach-Object { Q $_ }) -join ",") + ")"
      }
    }
  }
  New-Item -ItemType Directory -Force (Join-Path $Out "conv") | Out-Null
  Invoke-Locked "correctness" $lines
}
# Redesign V2 3.3 (P0): motion, lighting changes and cuts, each frame against the converged still image of the same pose
# (--path-time; several poses per reference run with --path-time-list). Frames are counted from the event: frame W + k - 1
# of a run is the k-th frame that shows the motion / the change / the cut (W = the warm-up, 300 still frames). Layers:
# final (the image), gi (view.giIrradiance), refl (view.reflection).
# The lamp each scene toggles (renderergate --list-lights): bath 5 = the ceiling lamp over the camera's view line
# (-6.75, 3.1, -1.25), train 7 = the lower-deck lamp 9 m behind the camera (0, 2.55, 29).
$lightIndex = @{ bath = "5"; train = "7" }
# Walking and running: along the bath's length (its camera looks down it); along the train car (its camera faces the side
# wall, so the move is sideways: --path-translate 0,v).
# The motion runs and their references use shading.motion_blur_shutter=0: a still reference has no blur, so the numbers
# judge the noise without the shutter hiding (or adding) anything - stricter than the game's 180 degree shutter.
$noBlur = @("--set", "shading.motion_blur_shutter=0")
$motionCases = [ordered]@{ rot90 = @{ bath = @("--path-rotate", "90"); train = @("--path-rotate", "90") }; rot180 = @{ bath = @("--path-rotate", "180"); train = @("--path-rotate", "180") };
  walk = @{ bath = @("--path-translate", "1.5"); train = @("--path-translate", "0,1.5") }; run = @{ bath = @("--path-translate", "4"); train = @("--path-translate", "0,4") } }
function Gate-Line([string]$name, [string[]]$a) { "Invoke-Step $(Q $name) $(Q $exe) @(" + (($a | ForEach-Object { Q $_ }) -join ",") + ")" }
$warm = if ($Quick) { 60 } else { 300 }
$still = if ($Quick) { 120 } else { 600 }
$long = if ($Quick) { 240 } else { 2000 }  # relight and cut: the same run continued to this frame is the reference
if ($Phases -contains "motion") {
  "== motion (Q-B: rotation 90 / 180 deg/s, walk 1.5 m/s, run 4 m/s: frames 1, 4, 8, 16, 32 of motion; still flicker)"
  $lines = @()
  $ks = if ($Quick) { @(1, 8) } else { @(1, 4, 8, 16, 32) }
  $refKs = if ($Quick) { @(8) } else { @(8, 16, 32) }
  foreach ($sc in $sceneNames) {
    foreach ($r in $resolutions) {
      foreach ($case in $motionCases.Keys) {
        $base = "motion\$($sc)_$($r)_$case"
        $frames = @($ks | ForEach-Object { $warm + $_ })
        $last = ($frames | Measure-Object -Maximum).Maximum
        $a = @("--scene", $scenes[$sc], "--resolution", $r, "--warmup-frames", "0", "--frames", "$($last + 1)", "--motion-start", "$warm") + $motionCases[$case][$sc] + $noBlur +
          @("--capture-output", (Join-Path $Out "$base.pfm"), "--capture-frames", ($frames -join ","), "--capture-layers", "final,gi,refl", "--frame-log", (Join-Path $Out "$($base)_frames.csv"))
        $lines += Gate-Line $base $a
        $times = @($refKs | ForEach-Object { "{0:0.######}" -f ($_ / 60.0) })
        $caps = @(0..($refKs.Count - 1) | ForEach-Object { ($_ + 1) * $still - 1 })
        $a = @("--scene", $scenes[$sc], "--resolution", $r, "--warmup-frames", "0", "--frames", "$($still * $refKs.Count)") + $motionCases[$case][$sc] + $noBlur +
          @("--path-time-list", ($times -join ","), "--segment-frames", "$still", "--capture-output", (Join-Path $Out "$($base)_ref.pfm"), "--capture-frames", ($caps -join ","), "--capture-layers", "final,gi,refl")
        $lines += Gate-Line "$($base)_ref" $a
      }
      # Still flicker (Q-D in the gate): 8 consecutive frames after the still warm-up.
      $base = "motion\$($sc)_$($r)_still"
      $a = @("--scene", $scenes[$sc], "--resolution", $r, "--warmup-frames", "0", "--frames", "$($still + 8)", "--capture-output", (Join-Path $Out "$base.pfm"), "--capture-frames", ((0..7 | ForEach-Object { $still + $_ }) -join ","))
      $lines += Gate-Line $base $a
    }
  }
  @{ warm = $warm; still = $still; ks = $ks; refKs = $refKs } | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $Out "motion\plan.json")
  Invoke-Locked "correctness" $lines
}
if ($Phases -contains "relight") {
  "== relight (Q-C: a lamp off, the sun turned 10 deg: frames 1, 4, 8, 16, 64, 256 after the change)"
  $lines = @()
  $ks = if ($Quick) { @(1, 8) } else { @(1, 4, 8, 16, 64, 256) }
  foreach ($sc in $sceneNames) {
    foreach ($r in $resolutions) {
      foreach ($ev in @("light", "sun")) {
        $base = "relight\$($sc)_$($r)_$ev"
        $at = { param($f) if ($ev -eq "light") { @("--light-toggle-at", "$f,$($lightIndex[$sc])") } else { @("--sun-step-at", "$f,10") } }
        # The reference is the same run continued (frame $long - 1): a run with the change from frame 0 converges to another
        # level in the bath - the GI cache's level depends on the camera's and the lights' history [measured, P1].
        $frames = @($ks | ForEach-Object { $warm + $_ - 1 })
        $a = @("--scene", $scenes[$sc], "--resolution", $r, "--warmup-frames", "0", "--frames", "$long") + (& $at $warm) +
          @("--capture-output", (Join-Path $Out "$base.pfm"), "--capture-frames", ((@($warm - 1) + $frames + @($long - 1)) -join ","), "--capture-layers", "final,gi,refl", "--frame-log", (Join-Path $Out "$($base)_frames.csv"))
        $lines += Gate-Line $base $a
      }
    }
  }
  @{ warm = $warm; still = $still; ks = $ks; long = $long } | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $Out "relight\plan.json")
  Invoke-Locked "correctness" $lines
}
if ($Phases -contains "cut") {
  "== cut (Q-A: a camera cut to the view turned 180 deg: frames 1, 4, 16, 64, 256 after the cut; two references for the noise floor)"
  $lines = @()
  $ks = if ($Quick) { @(1, 4) } else { @(1, 4, 16, 64, 256) }
  foreach ($sc in $sceneNames) {
    foreach ($r in $resolutions) {
      $base = "cut\$($sc)_$($r)"
      # The reference is the same run continued to frame $long - 1 (a cold start at the cut's pose converges to another
      # level in the bath [measured, P1]); a second identical run's last frame gives the noise floor.
      $frames = @($ks | ForEach-Object { $warm + $_ - 1 })
      $a = @("--scene", $scenes[$sc], "--resolution", $r, "--warmup-frames", "0", "--frames", "$long", "--path-rotate", "180", "--path-time", "0",
        "--cut-at", "$($warm):1.0", "--capture-output", (Join-Path $Out "$base.pfm"), "--capture-frames", (($frames + @($long - 1)) -join ","), "--capture-layers", "final,gi,refl",
        "--frame-log", (Join-Path $Out "$($base)_frames.csv"), "--gi-cache-stats", (Join-Path $Out "$($base)_gi.json"))
      $lines += Gate-Line $base $a
      $a = @("--scene", $scenes[$sc], "--resolution", $r, "--warmup-frames", "0", "--frames", "$long", "--path-rotate", "180", "--path-time", "0",
        "--cut-at", "$($warm):1.0", "--capture-output", (Join-Path $Out "$($base)_floor.pfm"), "--capture-frames", "$($long - 1)", "--capture-layers", "final,gi,refl",
        "--gi-cache-stats", (Join-Path $Out "$($base)_ref_gi.json"))
      $lines += Gate-Line "$($base)_floor" $a
    }
  }
  @{ warm = $warm; still = $still; ks = $ks; long = $long } | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $Out "cut\plan.json")
  Invoke-Locked "correctness" $lines
}
if ($Phases -contains "unity") {
  # After a deployment: the game project in Unity (batch mode, no window) - play-mode frames as the user sees them:
  # the first frames after the level's build, 8 still frames with their flicker map, walking frames. Uses the renderer
  # already deployed in that project. The project must be closed.
  "== unity ($GameProject, $GameScene)"
  if (-not $DryRun) { Test-Game }
  $open = Get-CimInstance Win32_Process -Filter "Name='Unity.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Replace('\', '/').ToLower().Contains($GameProject.Replace('\', '/').ToLower()) }
  if ($open) { throw "Unity가 $GameProject 를 열고 있다: 닫은 뒤 다시 실행한다" }
  $unity = "C:\Program Files\Unity\Hub\Editor\6000.6.0f1\Editor\Unity.exe"
  $u2 = Join-Path $GameProject "Artifacts\U2"
  if (Test-Path $u2) { Get-ChildItem $u2 -File | Remove-Item }
  foreach ($size in $(if ($Quick) { @("1920x1080") } else { @("1920x1080", "2560x1440") })) {
    $env:UNX_U2_SCENE = $GameScene; $env:UNX_U2_FIRST = "1,4,16,64"; $env:UNX_U2_SIZE = $size
    $log = Join-Path $Out "unity_$size.log"
    if (-not $DryRun) {
      & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "Tools\CI\GpuLock.ps1") -Track all -Kind correctness -TimeoutMinutes 40 -- $unity -batchmode -projectPath $GameProject -force-d3d12 -runTests -testPlatform EditMode -testFilter "UnravelNextBridge.Tests.UnravelNextU2CaptureTests" -testResults (Join-Path $Out "unity_$size.xml") -logFile $log *> (Join-Path $Out "unity_$($size)_lock.log")
      "unity $size exit $LASTEXITCODE"
      $dst = Join-Path $Out "unity\$size"
      New-Item -ItemType Directory -Force $dst | Out-Null
      if (Test-Path $u2) { Get-ChildItem $u2 -File | Move-Item -Destination $dst -Force }
    }
    Remove-Item Env:UNX_U2_SCENE, Env:UNX_U2_FIRST, Env:UNX_U2_SIZE -ErrorAction SilentlyContinue
  }
}
if ($Phases -contains "determinism") {
  "== determinism pairs (1080p)"
  $lines = @()
  foreach ($sc in $sceneNames) {
    foreach ($v in @(@{ n = "det"; a = @("--set", "debug.deterministic=true") }, @{ n = "def"; a = @() })) {
      foreach ($i in 1, 2) {
        $a = @("--scene", $scenes[$sc], "--resolution", "1920x1080", "--warmup-frames", "300") + $v.a + @("--capture-output", (Join-Path $Out "det\$($sc)_$($v.n)$i.pfm"))
        if ($Quick) { $a = @("--scene", $scenes[$sc], "--resolution", "1920x1080", "--warmup-frames", "60", "--frames", "60") + $v.a + @("--capture-output", (Join-Path $Out "det\$($sc)_$($v.n)$i.pfm")) }
        $lines += "Invoke-Step $(Q "det\$($sc)_$($v.n)$i") $(Q $exe) @(" + (($a | ForEach-Object { Q $_ }) -join ",") + ")"
      }
    }
  }
  Invoke-Locked "correctness" $lines
}
if ($Phases -contains "luminance") {
  "== luminance log (bath 1080p, 3000 frames)"
  $n = if ($Quick) { "200" } else { "3000" }
  $sc = if ($Quick) { "train" } else { "bath" }
  $a = @("--scene", $scenes[$sc], "--resolution", "1920x1080", "--frames", $n, "--luminance-log", (Join-Path $Out "lum\$($sc)_1080_default.csv"))
  Invoke-Locked "correctness" @("Invoke-Step $(Q "lum\$($sc)_1080_default") $(Q $exe) @(" + (($a | ForEach-Object { Q $_ }) -join ",") + ")")
}
if ($Phases -contains "timing") {
  "== timing (two rounds)"
  $lines = @()
  $timingRes = if ($Quick) { @("1920x1080") } else { @("1920x1080", "2560x1440", "3840x2160") }
  foreach ($round in 1, 2) {
    foreach ($sc in $sceneNames) {
      foreach ($r in $timingRes) {
        $a = @("--scene", $scenes[$sc], "--resolution", $r, "--out", (Join-Path $Out "timing\$($sc)_$($r)_$round")) + $(if ($Quick) { @("--frames", "60") } else { @() })
        $lines += "Invoke-Step $(Q "timing\$($sc)_$($r)_$round") $(Q $exe) @(" + (($a | ForEach-Object { Q $_ }) -join ",") + ")"
      }
    }
  }
  Invoke-Locked "timing" $lines
}
if ($Phases -contains "report") {
  "== report"
  & python (Join-Path $root "Tools\Verify\verify_analyze.py") $Out $gate
  if ($LASTEXITCODE -ne 0) { throw "보고서 작성 실패" }
  "보고서: $(Join-Path $Out 'SUMMARY_KO.md')"
  if (-not $KeepPfm) {
    $pfm = Get-ChildItem $Out -Recurse -Filter *.pfm -ErrorAction SilentlyContinue
    "PFM 삭제: $($pfm.Count)개, $([math]::Round((($pfm | Measure-Object Length -Sum).Sum) / 1GB, 1)) GB"
    $pfm | Remove-Item -ErrorAction SilentlyContinue
  }
}
if ($Phases -contains "publish") {
  # Evidence for the cloud session: everything in $Out except the large PFM captures, on branch local/verify-<sha>.
  "== publish to origin/local/verify-$sha"
  $w = Join-Path (Split-Path -Parent $root) "UnravelNext-verify"
  if (Test-Path $w) { & git -C $root worktree remove --force $w }
  & git -C $root worktree add -q --detach $w $Ref
  if ($LASTEXITCODE -ne 0) { throw "worktree 생성 실패" }
  $dst = Join-Path $w "Results\Local\Verify-$sha"
  & robocopy $Out $dst /E /XF *.pfm batch_*.ps1 /NFL /NDL /NJH /NJS /NP | Out-Null
  & git -C $w add -- "Results/Local/Verify-$sha"
  & git -C $w commit -q -m "Results/Local: local verification of $sha (Tools/Verify/Verify-CloudBranch.ps1; SUMMARY_KO.md)"
  if ($LASTEXITCODE -ne 0) { throw "커밋 실패" }
  & git -C $w push -q origin "HEAD:refs/heads/local/verify-$sha"
  $pushed = $LASTEXITCODE
  & git -C $root worktree remove --force $w
  if ($pushed -ne 0) { throw "푸시 실패" }
  "올림: origin/local/verify-$sha (Results/Local/Verify-$sha/SUMMARY_KO.md)"
}
