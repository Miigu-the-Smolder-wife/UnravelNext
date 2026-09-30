$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\p1b'
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
$off = @('--set', 'gi.update_tiers=false', '--set', 'gi.parent_prior=false', '--set', 'gi.relight_restart=false')
Step 'refl128_p1off' $bin 'unx_test_reflection_reflectionanalytic.exe' (@('--frames', '128') + $off)
Step 'refl128_start' $bin0 'unx_test_reflection_reflectionanalytic.exe' @('--frames', '128')
$scene = 'Results\R\GiInterior\bath_bt0_ev6.unxscene'
$cut = @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '364', '--path-rotate', '180', '--path-time', '0', '--cut-at', '300:1.0',
  '--capture-frames', '300,303,315,363', '--capture-layers', 'final,gi')
$nolocal = @('--set', 'gi.experiment_disable=128')
Step 'cut_bath_on128' $bin 'unx_gate_shadow_renderergate.exe' ($cut + @('--capture-output', "$out\cut_bath_on128.pfm") + $nolocal)
Step 'cut_bath_off128' $bin 'unx_gate_shadow_renderergate.exe' ($cut + @('--capture-output', "$out\cut_bath_off128.pfm") + $nolocal + $off)
Step 'cut_bath_ref128' $bin 'unx_gate_shadow_renderergate.exe' (@('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '600', '--path-rotate', '180',
  '--path-time', '1.0', '--capture-output', "$out\cut_bath_ref128.pfm", '--capture-frames', '599', '--capture-layers', 'final,gi') + $nolocal)
