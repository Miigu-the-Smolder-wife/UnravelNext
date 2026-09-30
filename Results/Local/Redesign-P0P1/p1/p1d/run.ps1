$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\p1d'
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
$scene = 'Results\R\GiInterior\bath_bt0_ev6.unxscene'
foreach ($c in @('8192', '16384')) {
  $cut = @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '364', '--path-rotate', '180', '--path-time', '0', '--cut-at', '300:1.0',
    '--capture-frames', '300,303,315,363', '--capture-layers', 'final,gi', '--set', "gi.experiment_disable=$c")
  Step "cut_bath_cap$c" $bin 'unx_gate_shadow_renderergate.exe' ($cut + @('--capture-output', "$out\cut_bath_cap$c.pfm"))
  Step "ref_bath_cap$c" $bin 'unx_gate_shadow_renderergate.exe' @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '600', '--path-rotate', '180',
    '--path-time', '1.0', '--capture-output', "$out\ref_bath_cap$c.pfm", '--capture-frames', '599', '--capture-layers', 'final,gi', '--set', "gi.experiment_disable=$c")
}
$cut = @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '364', '--path-rotate', '180', '--path-time', '0', '--cut-at', '300:1.0',
  '--capture-frames', '300,303,315,363', '--capture-layers', 'final,gi', '--frame-log', "$out\cut_bath_default_frames.csv", '--gi-cache-stats', "$out\cut_bath_default_gi.json")
Step "cut_bath_default" $bin 'unx_gate_shadow_renderergate.exe' ($cut + @('--capture-output', "$out\cut_bath_default.pfm"))
