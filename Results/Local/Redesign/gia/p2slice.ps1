$ErrorActionPreference = "Continue"
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\gia'
$bin = 'C:\Users\USER\UnravelNext-redesign\build\dev\bin'
$bin0 = 'C:\Users\USER\UnravelNext-redesign\build\all\bin'
$games = @('League of Legends', 'Remnant2-Win64-Shipping', 'Overwatch', 'VALORANT-Win64-Shipping', 'cs2', 'r5apex', 'FortniteClient-Win64-Shipping')
function Step($name, $dir, $exe, [string[]]$a) {
  if (Get-Process -Name $games -ErrorAction SilentlyContinue) { "$name : a game is running" }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $log = Join-Path $out ($name + '.log')
  & (Join-Path $dir $exe) @a *> $log
  $code = $LASTEXITCODE
  $hung = [bool](Select-String -Path $log -Pattern "DEVICE_(HUNG|REMOVED|RESET)|887A0005|887A0006")
  "$name exit $code $([int]$sw.Elapsed.TotalSeconds) s"
  if ($hung) { "STOP device removed in $name"; exit 87 }
}
$scene = 'Results\R\GiInterior\bath_bt0_ev6.unxscene'
# One lock hold (<= ~10 min) per call: P2_SCENE, P2_MODE (the turn: 'blend' = first, 'none' = second), P2_RES.
# With the dev2 build (the wide layer) ready: first turn = still wide + still none (+ rotations for the bath scenes), second
# turn = still blend (the maturity blend alone) + still wide without the temporal step (train: the rotations).
$out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\p2'
$scenes = @{ hall = 'C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene'; lounge = 'C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lounge_reference.unxscene'; train = 'C:\Users\USER\UnravelGames\TrainExorcist\Artifacts\Look\lounge_reference.unxscene' }
$off = @('--set', 'gi.screen_filter_adaptive=false', '--set', 'gi.screen_temporal_frames=0')
$modes = @{ blend = @(); none = $off; wide = @('--set', 'gi.screen_wide_filter=true'); widenotemporal = @('--set', 'gi.screen_wide_filter=true', '--set', 'gi.screen_temporal_frames=0'); centroid = ($off + @('--set', 'gi.anchor_centroid=true')); widec = @('--set', 'gi.screen_wide_filter=true', '--set', 'gi.miss_closure=true'); nonec = ($off + @('--set', 'gi.miss_closure=true')); widecp2 = @('--set', 'gi.screen_wide_filter=true', '--set', 'gi.miss_closure=true', '--set', 'gi.screen_wide_passes=2'); wideonlyc = @('--set', 'gi.screen_wide_filter=true', '--set', 'gi.miss_closure=true', '--set', 'gi.screen_wide_sigma_lo=0', '--set', 'gi.screen_wide_sigma_hi=0', '--set', 'gi.screen_temporal_frames=0'); adaptonly = @('--set', 'gi.screen_temporal_frames=0'); wideonly = @('--set', 'gi.screen_wide_filter=true', '--set', 'gi.screen_wide_sigma_lo=0', '--set', 'gi.screen_wide_sigma_hi=0', '--set', 'gi.screen_temporal_frames=0') }
$k = $env:P2_SCENE; $turn = $env:P2_MODE; $res = $env:P2_RES
$dev2 = 'C:\Users\USER\UnravelNext-redesign\build\dev2\bin'
$ready = (Test-Path "$dev2\unx_gate_shadow_renderergate.exe") -and [bool](Select-String -Path 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\gia\build_dev2.log' -Pattern 'build ok' -Quiet)
function Still($m, $b, $label) { if (-not $label) { $label = $m }; Step "still_${k}_${res}_$label" $b 'unx_gate_shadow_renderergate.exe' (@('--scene', $scenes[$k], '--resolution', $res, '--warmup-frames', '0', '--frames', '300', '--capture-frames', '1,3,4,15,16,299', '--capture-layers', 'final,gi', '--capture', "$out\still_${k}_${res}_$label.pfm") + $modes[$m]) }
function Rot($m, $b) { Step "rot_${k}_$m" $b 'unx_gate_shadow_renderergate.exe' (@('--scene', $scenes[$k], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '180', '--path-rotate', '90', '--motion-start', '60', '--capture-frames', '59,63,75,120,179', '--capture-layers', 'final,gi', '--capture', "$out\rot_${k}_$m.pfm") + $modes[$m]) }
if (-not $ready) {
  "dev2 not ready: the dev build's $turn"
  Still $turn $bin
  if ($res -eq '1920x1080') { Rot $turn $bin }
} elseif ($turn -eq 'vsm') {
  # defect queue 1: the VSM raster fix's correctness with the per-face and leaf-cut bounds (shadow layer f0 = f300, no overflow)
  $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\raster'
  Step 'face_lounge' $dev2 'unx_gate_shadow_renderergate.exe' @('--scene', $scenes['lounge'], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '301', '--capture-frames', '0,300', '--capture-layers', 'shadow', '--set', 'debug.deterministic=true', '--capture', "$out\face_lounge.pfm")
  Step 'face_train' $dev2 'unx_gate_shadow_renderergate.exe' @('--scene', $scenes['train'], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '301', '--capture-frames', '0,60,300', '--capture-layers', 'shadow', '--set', 'debug.deterministic=true', '--capture', "$out\face_train.pfm")
  Step 'v_clusterbuilder' $dev2 'unx_test_clusterbuilder.exe' @()
  Step 'v_visibilitytests' $dev2 'unx_test_visibility_visibilitytests.exe' @()
  Step 'v_vsmtests' $dev2 'unx_test_shadow_vsmtests.exe' @()
  Step 'v_localshadowtests' $dev2 'unx_test_shadow_localshadowtests.exe' @()
} elseif ($turn -eq 'vsm2') {
  # queue 1b: the per-view LOD bound of the request packing (shadow layer f0 = f300, no overflow, request counts in the logs)
  $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\raster'
  Step 'lod_lounge' $dev2 'unx_gate_shadow_renderergate.exe' @('--scene', $scenes['lounge'], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '301', '--capture-frames', '0,300', '--capture-layers', 'shadow', '--set', 'debug.deterministic=true', '--capture', "$out\lod_lounge.pfm")
  Step 'lod_train' $dev2 'unx_gate_shadow_renderergate.exe' @('--scene', $scenes['train'], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '301', '--capture-frames', '0,60,300', '--capture-layers', 'shadow', '--set', 'debug.deterministic=true', '--capture', "$out\lod_train.pfm")
  Step 'lod_hall' $dev2 'unx_gate_shadow_renderergate.exe' @('--scene', $scenes['hall'], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '301', '--capture-frames', '0,300', '--capture-layers', 'shadow', '--set', 'debug.deterministic=true', '--capture', "$out\lod_hall.pfm")
  Step 'lod_visibilitytests' $dev2 'unx_test_visibility_visibilitytests.exe' @()
  Step 'lod_vsmtests' $dev2 'unx_test_shadow_vsmtests.exe' @()
  Step 'lod_localshadowtests' $dev2 'unx_test_shadow_localshadowtests.exe' @()
} elseif ($turn -eq 'vsmtime') {
  # request counts and pass times: per-view LOD bound / leaf bound / no split (k = scene)
  $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\rastertime3'
  New-Item -ItemType Directory -Force $out | Out-Null
  $vm = [ordered]@{ lod = @(); leaf = @('--set', 'shadow.vsm.raster_lod_bound=false'); old = @('--set', 'visibility.raster_amplification=false', '--set', 'shadow.vsm.raster_split=false') }
  foreach ($m in $vm.Keys) { Step "rt3_${k}_${res}_$m" $dev2 'unx_gate_shadow_renderergate.exe' (@('--scene', $scenes[$k], '--resolution', $res, '--warmup-frames', '120', '--frames', '300', '--out', "$out\${k}_${res}_$m") + $vm[$m]) }
} elseif ($turn -eq 'tests') {
  $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\p2tests'
  New-Item -ItemType Directory -Force $out | Out-Null
  foreach ($t in 'unx_test_gi_gianalytic', 'unx_test_reflection_reflectionanalytic', 'unx_test_host_hostmotion', 'unx_test_shading_shadingtests', 'unx_test_volume_volumetests', 'unx_test_shadow_froxeltests') { Step "t_$t" $dev2 "$t.exe" @() }
} elseif ($turn -eq 'lounge2') {
  Still 'widec' $dev2 'widec3'; Still 'widecp2' $dev2; Still 'nonec' $dev2 'nonec2'; Still 'wideonlyc' $dev2; Rot 'widec' $dev2; Rot 'nonec' $dev2
} elseif ($turn -eq 'closure') {
  Still 'widec' $dev2; Still 'widec' $dev2 'widec2'; Still 'nonec' $dev2; Rot 'widec' $dev2
} elseif ($turn -eq 'blend') {
  if ($k -eq 'train') { Still 'wide' $dev2; Still 'none' $dev2; Still 'widec' $dev2 }
  else {
    Still 'wide' $dev2; Still 'none' $dev2; Still 'wideonly' $dev2; Still 'widec' $dev2
    if ($res -eq '1920x1080') { Rot 'wide' $dev2; Rot 'none' $dev2; Rot 'widec' $dev2 }
  }
} else {
  if ($k -eq 'train') { Rot 'wide' $dev2; Rot 'none' $dev2; Rot 'widec' $dev2 }
  else { Still 'nonec' $dev2; Still 'widec' $dev2 'widec2'; Still 'centroid' $dev2 }
}
