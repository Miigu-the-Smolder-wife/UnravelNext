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
  [string[]]$Phases = @("build", "tests", "caps", "determinism", "luminance", "timing", "report"),
  [switch]$Quick,
  [switch]$DryRun   # write each GPU batch and parse it, run nothing on the GPU (checks the script itself)
)
$ErrorActionPreference = "Continue"  # (native tools write to stderr; failures are checked by exit code and thrown explicitly)
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$gate = Join-Path (Split-Path -Parent $root) "UnravelNext-gate"
$games = @("League of Legends", "Remnant2-Win64-Shipping", "Overwatch", "VALORANT-Win64-Shipping", "cs2", "r5apex", "FortniteClient-Win64-Shipping")
if ($Phases.Count -eq 1 -and $Phases[0] -match ",") { $Phases = $Phases[0] -split "," }

function Test-Game {
  $g = Get-Process -Name $games -ErrorAction SilentlyContinue
  if ($g) { throw "게임 실행 중($($g[0].Name)): GPU 작업을 하지 않는다. 게임을 끝낸 뒤 다시 실행한다." }
}
$gpuPhases = @($Phases | Where-Object { $_ -in @("build", "tests", "caps", "determinism", "luminance", "timing") }).Count -gt 0
if ($gpuPhases -and -not $DryRun) { Test-Game }
& git -C $root fetch -q origin 2>$null
$sha = (& git -C $root rev-parse --short $Ref).Trim()
if (-not $Out) { $Out = Join-Path $root "Results\Local\Verify-$sha" }
foreach ($d in "", "tests", "caps", "det", "lum", "timing") { New-Item -ItemType Directory -Force (Join-Path $Out $d) | Out-Null }
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
    'function R($name, $file, [string[]]$a) {',
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
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "Tools\CI\Build.ps1") -Track all -Committed -Ref $Ref *> $log
  if ($LASTEXITCODE -ne 0) {
    # A build stopped earlier can leave 0-byte object files (LNK1136); remove them once and build again.
    $empty = Get-ChildItem (Join-Path $gate "build\all") -Recurse -Include *.obj -ErrorAction SilentlyContinue | Where-Object { $_.Length -eq 0 }
    if ($empty) { $empty | Remove-Item; & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "Tools\CI\Build.ps1") -Track all -Committed -Ref $Ref *> $log }
    if ($LASTEXITCODE -ne 0) { throw "빌드 실패: $log (FAILED 줄과 error 줄을 클라우드 세션에 전달한다)" }
  }
  (& git -C $gate rev-parse --short HEAD).Trim() | Set-Content (Join-Path $Out "gate_sha.txt")
}
# The committed build leaves the gate worktree clean, which removes the scene links: link them again every time.
foreach ($s in $scenes.Values) {
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
  Invoke-Locked "correctness" ($tests | ForEach-Object { "R $(Q "tests\$_") $(Q (Join-Path $gate "build\all\bin\$_.exe")) @()" })
}
if ($Phases -contains "caps") {
  "== captures (native vs upscaled)"
  $lines = @()
  foreach ($sc in $sceneNames) {
    foreach ($r in $resolutions) {
      $modes = @(@{ m = "static"; a = @() })
      if ($sc -eq "train") { $modes += @{ m = "moving"; a = @("--moving") }; $modes += @{ m = "sun"; a = @("--sun-deg-per-s", "20") } }
      foreach ($md in $modes) {
        $base = "$($sc)_$($r)_$($md.m)"
        $common = @("--scene", $scenes[$sc], "--resolution", $r) + $md.a + $frames
        $lines += "R $(Q "caps\$($base)_native") $(Q $exe) @(" + ((($common + @("--capture", (Join-Path $Out "caps\$($base)_native.pfm"))) | ForEach-Object { Q $_ }) -join ",") + ")"
        $lines += "R $(Q "caps\$($base)_up") $(Q $exe) @(" + ((($common + @("--capture-output", (Join-Path $Out "caps\$($base)_up.pfm"))) | ForEach-Object { Q $_ }) -join ",") + ")"
      }
    }
  }
  Invoke-Locked "correctness" $lines
}
if ($Phases -contains "determinism") {
  "== determinism pairs (1080p)"
  $lines = @()
  foreach ($sc in $sceneNames) {
    foreach ($v in @(@{ n = "det"; a = @("--set", "debug.deterministic=true") }, @{ n = "def"; a = @() })) {
      foreach ($i in 1, 2) {
        $a = @("--scene", $scenes[$sc], "--resolution", "1920x1080", "--warmup-frames", "300") + $v.a + @("--capture-output", (Join-Path $Out "det\$($sc)_$($v.n)$i.pfm"))
        if ($Quick) { $a = @("--scene", $scenes[$sc], "--resolution", "1920x1080", "--warmup-frames", "60", "--frames", "60") + $v.a + @("--capture-output", (Join-Path $Out "det\$($sc)_$($v.n)$i.pfm")) }
        $lines += "R $(Q "det\$($sc)_$($v.n)$i") $(Q $exe) @(" + (($a | ForEach-Object { Q $_ }) -join ",") + ")"
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
  Invoke-Locked "correctness" @("R $(Q "lum\$($sc)_1080_default") $(Q $exe) @(" + (($a | ForEach-Object { Q $_ }) -join ",") + ")")
}
if ($Phases -contains "timing") {
  "== timing (two rounds)"
  $lines = @()
  $timingRes = if ($Quick) { @("1920x1080") } else { @("1920x1080", "2560x1440", "3840x2160") }
  foreach ($round in 1, 2) {
    foreach ($sc in $sceneNames) {
      foreach ($r in $timingRes) {
        $a = @("--scene", $scenes[$sc], "--resolution", $r, "--out", (Join-Path $Out "timing\$($sc)_$($r)_$round")) + $(if ($Quick) { @("--frames", "60") } else { @() })
        $lines += "R $(Q "timing\$($sc)_$($r)_$round") $(Q $exe) @(" + (($a | ForEach-Object { Q $_ }) -join ",") + ")"
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
