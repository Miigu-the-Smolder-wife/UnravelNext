$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\p1e'
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
foreach ($sc in @('bath', 'train')) {
  $scene = "Results\R\GiInterior\$($sc)_$(if ($sc -eq 'bath') { 'bt0' } else { 'v0' })_ev6.unxscene"
  $cut = @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '364', '--path-rotate', '180', '--path-time', '0', '--cut-at', '300:1.0',
    '--capture-frames', '300,303,315,363', '--capture-layers', 'final,gi', '--frame-log', "$out\cut_$($sc)_frames.csv", '--gi-cache-stats', "$out\cut_$($sc)_gi.json")
  Step "cut_$sc" $bin 'unx_gate_shadow_renderergate.exe' ($cut + @('--capture-output', "$out\cut_$sc.pfm"))
  $ref = @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '600', '--path-rotate', '180', '--path-time', '1.0', '--capture-frames', '599', '--capture-layers', 'final,gi')
  Step "ref_$sc" $bin 'unx_gate_shadow_renderergate.exe' ($ref + @('--capture-output', "$out\ref_$sc.pfm", '--gi-cache-stats', "$out\ref_$($sc)_gi.json"))
  Step "ref_$($sc)_nofp" $bin 'unx_gate_shadow_renderergate.exe' ($ref + @('--capture-output', "$out\ref_$($sc)_nofp.pfm", '--set', 'gi.hit_light_footprint=false'))
}
Step 'gianalytic' $bin 'unx_test_gi_gianalytic.exe' @()
