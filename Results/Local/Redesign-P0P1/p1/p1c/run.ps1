$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\p1c'
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
Step 'gianalytic' $bin 'unx_test_gi_gianalytic.exe' @()
$scene = 'Results\R\GiInterior\bath_bt0_ev6.unxscene'
$cut = @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '364', '--path-rotate', '180', '--path-time', '0', '--cut-at', '300:1.0',
  '--capture-frames', '300,303,315,363', '--capture-layers', 'final,gi', '--frame-log', "$out\cut_bath_all_frames.csv")
$all = @('--set', 'gi.experiment_disable=4096')
Step 'cut_bath_all' $bin 'unx_gate_shadow_renderergate.exe' ($cut + @('--capture-output', "$out\cut_bath_all.pfm") + $all)
Step 'cut_bath_ref_all' $bin 'unx_gate_shadow_renderergate.exe' (@('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '600', '--path-rotate', '180',
  '--path-time', '1.0', '--capture-output', "$out\cut_bath_ref_all.pfm", '--capture-frames', '599', '--capture-layers', 'final,gi', '--frame-log', "$out\ref_all_frames.csv") + $all)
Step 'cut_bath_ref_default' $bin 'unx_gate_shadow_renderergate.exe' (@('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '600', '--path-rotate', '180',
  '--path-time', '1.0', '--capture-output', "$out\cut_bath_ref_default.pfm", '--capture-frames', '599', '--capture-layers', 'final,gi', '--frame-log', "$out\ref_default_frames.csv"))
