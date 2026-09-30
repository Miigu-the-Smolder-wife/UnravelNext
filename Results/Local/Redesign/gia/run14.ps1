$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\gia'
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
foreach ($sc in @(@('bath', 'Results\R\GiInterior\bath_bt0_ev6.unxscene'), @('train', 'Results\R\GiInterior\train_v0_ev6.unxscene'))) {
  foreach ($k in @('1.0', '0.5', '0.25')) {
    $c = @('--scene', $sc[1], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '600', '--strip-clearcoat', '--set', 'gi.experiment_disable=32768', '--set', "gi.hit_light_footprint_scale=$k", '--out', "$out\json")
    Step "audit_$($sc[0])_$k" $bin 'unx_gate_shadow_renderergate.exe' $c
  }
}
