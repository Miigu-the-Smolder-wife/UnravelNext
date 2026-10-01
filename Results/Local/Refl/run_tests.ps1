# S2 correctness tests of the reflection layers, one after another inside one GpuLock (-Kind correctness):
#   powershell -File Tools/CI/GpuLock.ps1 -Track S2 -Kind correctness -- powershell -NoProfile -File Results/Local/Refl/run_tests.ps1 -Tag t1
# Each test's full log goes to Results/Local/Refl/tests/<Tag>/; the run stops at a device removal (exit 87).
param([string]$Tag = "t", [string[]]$Only = @())
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)))
Set-Location $root
$bin = Join-Path $root "build\all\bin"
$out = Join-Path $root "Results\Local\Refl\tests\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$on = @("--set", "reflection.layers=true")
$tests = @(
  @{ name = "reflectionanalytic_prev"; exe = "unx_test_reflection_reflectionanalytic.exe"; args = @("--set", "reflection.layers=false", "--set", "reflection.hit_cone_lobes=false") },
  @{ name = "reflectionanalytic_off"; exe = "unx_test_reflection_reflectionanalytic.exe"; args = @("--set", "reflection.layers=false") },
  @{ name = "reflectionanalytic_oriented"; exe = "unx_test_reflection_reflectionanalytic.exe"; args = @("--set", "reflection.layers=false", "--set", "reflection.hit_oriented_lights=true") },
  @{ name = "reflectionanalytic_on"; exe = "unx_test_reflection_reflectionanalytic.exe"; args = $on },
  @{ name = "reflectionanalytic_on_nohistory"; exe = "unx_test_reflection_reflectionanalytic.exe"; args = $on + @("--set", "reflection.layer_history_frames=1") },
  @{ name = "planarmirror"; exe = "unx_test_reflection_planarmirror.exe"; args = @() },
  @{ name = "gianalytic"; exe = "unx_test_gi_gianalytic.exe"; args = @() },
  @{ name = "hostmotion"; exe = "unx_test_host_hostmotion.exe"; args = @() }
)
$summary = @()
foreach ($t in $tests) {
  if ($Only.Count -gt 0 -and $Only -notcontains $t.name) { continue }
  $log = Join-Path $out ($t.name + ".log")
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & (Join-Path $bin $t.exe) @($t.args) *> $log
  $code = $LASTEXITCODE
  $text = Get-Content $log -Raw
  $tdr = $text -match "DEVICE_HUNG|DEVICE_REMOVED|0x887A0006|0x887A0005"
  $line = "{0}: exit {1}, {2:N0} s{3}" -f $t.name, $code, $sw.Elapsed.TotalSeconds, $(if ($tdr) { " TDR" } else { "" })
  $line
  $summary += $line
  if ($tdr -or $code -eq 87) { "device removal: stopping"; $summary | Set-Content (Join-Path $out "summary.txt") -Encoding utf8; exit 87 }
}
$summary | Set-Content (Join-Path $out "summary.txt") -Encoding utf8
exit 0
