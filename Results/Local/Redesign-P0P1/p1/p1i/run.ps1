$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\p1i'
$bin = 'C:\Users\USER\UnravelNext-redesign\build\dev\bin'
$bin0 = 'C:\Users\USER\UnravelNext-redesign\build\all\bin'
$games = @('League of Legends', 'Remnant2-Win64-Shipping', 'Overwatch', 'VALORANT-Win64-Shipping', 'cs2', 'r5apex', 'FortniteClient-Win64-Shipping')
function Step($name, $dir, $exe, [string[]]$a) {
  if (Get-Process -Name $games -ErrorAction SilentlyContinue) { "STOP game"; exit 3 }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $log = Join-Path $out ($name + '.log')
  & (Join-Path $dir $exe) @a *> $log
  $code = $LASTEXITCODE
  $hung = [bool](Select-String -Path $log -Pattern "DEVICE_(HUNG|REMOVED|RESET)|887A0005|887A0006")
  "$name exit $code $([int]$sw.Elapsed.TotalSeconds) s"
  if ($hung) { "STOP device removed in $name"; exit 87 }
}
$off = @('--set', 'gi.update_tiers=false', '--set', 'gi.parent_prior=false')
Step 'gianalytic_determinism' $bin 'unx_test_gi_gianalytic.exe' @('--determinism')
Step 'gianalytic' $bin 'unx_test_gi_gianalytic.exe' @()
Step 'hostmotion' $bin 'unx_test_host_hostmotion.exe' @()
foreach ($i in 1, 2, 3) {
  Step "refl_on$i" $bin 'unx_test_reflection_reflectionanalytic.exe' @()
  Step "refl_off$i" $bin 'unx_test_reflection_reflectionanalytic.exe' $off
}
foreach ($sc in @('bath', 'train')) {
  $scene = "Results\R\GiInterior\$($sc)_$(if ($sc -eq 'bath') { 'bt0' } else { 'v0' })_ev6.unxscene"
  foreach ($i in 1, 2) { Step "det_$($sc)$i" $bin 'unx_gate_shadow_renderergate.exe' @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '300', '--set', 'debug.deterministic=true', '--capture-output', "$out\det_$($sc)$i.pfm") }
  $light = if ($sc -eq 'bath') { '5' } else { '7' }
  foreach ($v in @(@{ n = 'on'; a = @() }, @{ n = 'off'; a = $off })) {
    Step "relight_$($sc)_$($v.n)" $bin 'unx_gate_shadow_renderergate.exe' (@('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '2000', '--light-toggle-at', "300,$light",
      '--capture-output', "$out\relight_$($sc)_$($v.n).pfm", '--capture-frames', '299,300,303,307,315,363,555,1999', '--frame-log', "$out\relight_$($sc)_$($v.n)_frames.csv") + $v.a)
    Step "cut_$($sc)_$($v.n)" $bin 'unx_gate_shadow_renderergate.exe' (@('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '2000', '--path-rotate', '180', '--path-time', '0', '--cut-at', '300:1.0',
      '--capture-output', "$out\cut_$($sc)_$($v.n).pfm", '--capture-frames', '300,303,315,363,555,1999', '--frame-log', "$out\cut_$($sc)_$($v.n)_frames.csv") + $v.a)
  }
}
