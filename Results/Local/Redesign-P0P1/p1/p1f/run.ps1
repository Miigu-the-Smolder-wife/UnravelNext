$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\p1f'
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
Step 'cold' $bin 'unx_gate_shadow_renderergate.exe' @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '2000', '--path-rotate', '180', '--path-time', '1.0', '--luminance-log', "$out\cold.csv", '--capture-output', "$out\cold.pfm", '--capture-frames', '599,1999')
Step 'cut' $bin 'unx_gate_shadow_renderergate.exe' @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '2000', '--path-rotate', '180', '--path-time', '0', '--cut-at', '300:1.0', '--luminance-log', "$out\cut.csv", '--capture-output', "$out\cut.pfm", '--capture-frames', '363,899,1999')
