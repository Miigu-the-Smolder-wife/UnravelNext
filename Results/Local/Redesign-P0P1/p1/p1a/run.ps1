$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\p1a'
$bin = 'C:\Users\USER\UnravelNext-redesign\build\dev\bin'
$games = @('League of Legends', 'Remnant2-Win64-Shipping', 'Overwatch', 'VALORANT-Win64-Shipping', 'cs2', 'r5apex', 'FortniteClient-Win64-Shipping')
function Step($name, $exe, [string[]]$a) {
  if (Get-Process -Name $games -ErrorAction SilentlyContinue) { "STOP game"; exit 3 }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $log = Join-Path $out ($name + '.log')
  & (Join-Path $bin $exe) @a *> $log
  $code = $LASTEXITCODE
  $hung = [bool](Select-String -Path $log -Pattern "DEVICE_(HUNG|REMOVED|RESET)|887A0005|887A0006")
  "$name exit $code $([int]$sw.Elapsed.TotalSeconds) s"
  if ($hung) { "STOP device removed in $name"; exit 87 }
}
Step 'gianalytic' 'unx_test_gi_gianalytic.exe' @()
Step 'reflectionanalytic_128' 'unx_test_reflection_reflectionanalytic.exe' @('--frames', '128')
foreach ($sc in @('bath', 'train')) {
  $scene = "Results\R\GiInterior\$($sc)_$(if ($sc -eq 'bath') { 'bt0' } else { 'v0' })_ev6.unxscene"
  foreach ($v in @(@{ n = 'on'; a = @() }, @{ n = 'off'; a = @('--set', 'gi.update_tiers=false', '--set', 'gi.parent_prior=false', '--set', 'gi.relight_restart=false') })) {
    $a = @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '364', '--path-rotate', '180', '--path-time', '0', '--cut-at', '300:1.0',
      '--capture-output', "$out\cut_$($sc)_$($v.n).pfm", '--capture-frames', '300,303,315,363', '--capture-layers', 'final,gi', '--frame-log', "$out\cut_$($sc)_$($v.n)_frames.csv",
      '--gi-cache-stats', "$out\cut_$($sc)_$($v.n)_gi.json") + $v.a
    Step "cut_$($sc)_$($v.n)" 'unx_gate_shadow_renderergate.exe' $a
  }
  $a = @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '600', '--path-rotate', '180', '--path-time', '1.0',
    '--capture-output', "$out\cut_$($sc)_ref.pfm", '--capture-frames', '599', '--capture-layers', 'final,gi', '--gi-cache-stats', "$out\cut_$($sc)_ref_gi.json")
  Step "cut_$($sc)_ref" 'unx_gate_shadow_renderergate.exe' $a
}
