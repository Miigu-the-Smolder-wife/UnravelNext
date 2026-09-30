$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\p1k'
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
Step 'refl1' $bin 'unx_test_reflection_reflectionanalytic.exe' @()
Step 'refl2' $bin 'unx_test_reflection_reflectionanalytic.exe' @()
Step 'refl128' $bin 'unx_test_reflection_reflectionanalytic.exe' @('--frames', '128')
Step 'gianalytic' $bin 'unx_test_gi_gianalytic.exe' @()
Step 'gianalytic_determinism' $bin 'unx_test_gi_gianalytic.exe' @('--determinism')
Step 'hostmotion' $bin 'unx_test_host_hostmotion.exe' @()
Step 'shadingtests' $bin 'unx_test_shading_shadingtests.exe' @()
$scene = 'Results\R\GiInterior\bath_bt0_ev6.unxscene'
foreach ($i in 1, 2) { Step "det$i" $bin 'unx_gate_shadow_renderergate.exe' @('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '300', '--set', 'debug.deterministic=true', '--capture-output', "$out\det$i.pfm") }
foreach ($v in @(@{ n = 'inv'; a = @() }, @{ n = 'noinv'; a = @('--set', 'gi.light_invalidation=false') })) {
  Step "relight_$($v.n)" $bin 'unx_gate_shadow_renderergate.exe' (@('--scene', $scene, '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '2000', '--light-toggle-at', '300,5',
    '--capture-output', "$out\relight_$($v.n).pfm", '--capture-frames', '299,300,303,307,315,363,555,1999') + $v.a)
}
