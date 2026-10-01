$ErrorActionPreference = "Continue"
# One GPU lock hold (<= ~10 min) of the post-game verification (CLOUD_BRIEF "세션 13 이어서 (5)"). Environment: PG_TURN,
# PG_SCENE (lounge | hall | train), PG_RES. Called by postgame.ps1 through Tools\CI\GpuLock.ps1; the binaries are
# build\dev2 (rebuilt from the committed tree before the queue starts). CPU-only tests are not run here.
Set-Location 'C:\Users\USER\UnravelNext-redesign'
$bin = 'C:\Users\USER\UnravelNext-redesign\build\dev2\bin'
$p2 = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\p2'
$out = $p2
function Step($name, $exe, [string[]]$a) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $log = Join-Path $out ($name + '.log')
  & (Join-Path $bin $exe) @a *> $log
  $code = $LASTEXITCODE
  $hung = [bool](Select-String -Path $log -Pattern "DEVICE_(HUNG|REMOVED|RESET)|887A0005|887A0006")
  "$name exit $code $([int]$sw.Elapsed.TotalSeconds) s"
  if ($hung) { "STOP device removed in $name"; exit 87 }
}
$scenes = @{ hall = 'C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene'; lounge = 'C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lounge_reference.unxscene'; train = 'C:\Users\USER\UnravelGames\TrainExorcist\Artifacts\Look\lounge_reference.unxscene'; lobby = 'C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene' }
function S([string[]]$kv) { $r = @(); foreach ($x in $kv) { $r += @('--set', $x) }; return $r }
$filterOff = S @('gi.screen_filter_adaptive=false', 'gi.screen_temporal_frames=0', 'gi.screen_wide_filter=false')
$coldOff = S @('gi.bounce_visibility=false', 'gi.miss_closure=false')
$modes = @{
  full = @()                                   # the defaults: wide layer, bounce visibility, closure
  none = ($filterOff + $coldOff)               # the path before P2 and the cold-start change
  nofilter = $filterOff                        # cold-start defaults, no reconstruction
  coldoff = $coldOff                           # reconstruction, the previous bounce reads
  visonly = (S @('gi.miss_closure=false'))
  closureonly = (S @('gi.bounce_visibility=false'))
  passes2 = (S @('gi.screen_wide_passes=2'))
  wideonly = (S @('gi.screen_wide_sigma_lo=0', 'gi.screen_wide_sigma_hi=0', 'gi.screen_temporal_frames=0'))
  acc = (S @('gi.hit_accumulator=true'))
  accnone = ($filterOff + (S @('gi.hit_accumulator=true')))
  # the coordinator's first comparison on the integrated branch: every noise change on / the paths before them
  allon = (S @('gi.hit_accumulator=true', 'reflection.layers=true', 'reflection.layer_mirror_lobe=true'))
  allon_noclosure = (S @('gi.hit_accumulator=true', 'reflection.layers=true', 'reflection.layer_mirror_lobe=true', 'gi.miss_closure=false'))
  allon_noacc = (S @('reflection.layers=true', 'reflection.layer_mirror_lobe=true'))
  allon_nolayers = (S @('gi.hit_accumulator=true'))
  allon_passes1 = (S @('gi.hit_accumulator=true', 'reflection.layers=true', 'reflection.layer_mirror_lobe=true', 'gi.screen_wide_passes=1'))
  alloff = ($filterOff + $coldOff + (S @('reflection.layers=false', 'reflection.hit_cone_lobes=false', 'reflection.hit_accumulator=false')))
}
$k = $env:PG_SCENE; $res = $env:PG_RES; $turn = $env:PG_TURN
function Still($m, $label) { if (-not $label) { $label = $m }; Step "still_${k}_${res}_$label" 'unx_gate_shadow_renderergate.exe' (@('--scene', $scenes[$k], '--resolution', $res, '--warmup-frames', '0', '--frames', '300', '--capture-frames', '1,4,16,299', '--capture-layers', 'final,gi', '--capture', "$p2\still_${k}_${res}_$label.pfm") + $modes[$m]) }
function Rot($m, $label) { if (-not $label) { $label = $m }; Step "rot_${k}_$label" 'unx_gate_shadow_renderergate.exe' (@('--scene', $scenes[$k], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '180', '--path-rotate', '90', '--motion-start', '60', '--capture-frames', '59,63,120,179', '--capture-layers', 'final', '--capture', "$p2\rot_${k}_$label.pfm") + $modes[$m]) }
switch ($turn) {
  'acc1' {
    # first run of the new kernel (GiAccFold) and of GiTrace with the pool: short, then the energy audit (64-frame window rule: 600 frames)
    Still 'acc'
    $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\acc'
    Step 'pool_audit' 'unx_gate_shadow_renderergate.exe' (@('--scene', 'Results\R\GiInterior\bath_bt0_ev6.unxscene', '--strip-clearcoat', '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '600', '--out', "$out\json_pool") + (S @('gi.hit_accumulator=true', 'gi.experiment_disable=32768')))
    Step 'pool_t9' 'unx_test_gi_gianalytic.exe' (@('--only-light-near') + (S @('gi.hit_accumulator=true')))
  }
  'acc2' {
    $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\acc'
    foreach ($i in 1..4) { Step "pool_run_$i" 'unx_gate_shadow_renderergate.exe' (@('--scene', 'Results\R\GiInterior\bath_bt0_ev6.unxscene', '--strip-clearcoat', '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '2000', '--capture-frames', '1999', '--capture', "$out\pool_run_$i.pfm") + (S @('gi.hit_accumulator=true'))) }
  }
  'accdiag' {
    # GiAnalytic 9 (light near surfaces) with the pool: which parameter the lampshade line's -23 % follows
    $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\acc'
    $v = [ordered]@{ pool = @(); min8 = (S @('gi.hit_accumulator_min_samples=8')); alpha32 = (S @('gi.hit_accumulator_alpha=0.03125')); min8alpha32 = (S @('gi.hit_accumulator_min_samples=8', 'gi.hit_accumulator_alpha=0.03125')); fine05 = (S @('gi.hit_accumulator_fine_scale=0.5')); entry = (S @('gi.hit_accumulator_pool=false', 'gi.hit_accumulator_frame=true', 'gi.hit_accumulator_cell_scale=0.25', 'gi.hit_accumulator_ratio=true')) }
    foreach ($n in $v.Keys) { Step "t9_$n" 'unx_test_gi_gianalytic.exe' (@('--only-light-near') + (S @('gi.hit_accumulator=true')) + $v[$n]) }
  }
  'acc3' { Still 'accnone'; Still 'nofilter' 'nofilter_b'; Rot 'acc' }
  'vsm2' {
    $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\raster'
    foreach ($s in 'lounge', 'train', 'hall') {
      $frames = if ($s -eq 'train') { '0,60,300' } else { '0,300' }
      Step "lod_$s" 'unx_gate_shadow_renderergate.exe' @('--scene', $scenes[$s], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '301', '--capture-frames', $frames, '--capture-layers', 'shadow', '--set', 'debug.deterministic=true', '--capture', "$out\lod_$s.pfm")
    }
    Step 'lod_visibilitytests' 'unx_test_visibility_visibilitytests.exe' @()
    Step 'lod_vsmtests' 'unx_test_shadow_vsmtests.exe' @()
    Step 'lod_localshadowtests' 'unx_test_shadow_localshadowtests.exe' @()
  }
  'vsmtime' {
    $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\rastertime3'
    New-Item -ItemType Directory -Force $out | Out-Null
    $vm = [ordered]@{ lod = @(); leaf = (S @('shadow.vsm.raster_lod_bound=false')); old = (S @('visibility.raster_amplification=false', 'shadow.vsm.raster_split=false')) }
    foreach ($m in $vm.Keys) { Step "rt3_${k}_${res}_$m" 'unx_gate_shadow_renderergate.exe' (@('--scene', $scenes[$k], '--resolution', $res, '--warmup-frames', '120', '--frames', '300', '--out', "$out\${k}_${res}_$m") + $vm[$m]) }
  }
  'tests' {
    $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\p2tests'
    New-Item -ItemType Directory -Force $out | Out-Null
    foreach ($t in 'unx_test_gi_gianalytic', 'unx_test_reflection_reflectionanalytic', 'unx_test_host_hostmotion', 'unx_test_shading_shadingtests', 'unx_test_volume_volumetests', 'unx_test_shadow_froxeltests') { Step "t_$t" "$t.exe" @() }
  }
  'isolate' {
    # which change the frame 3-4 block pattern of the train's GI layer follows: all on with one group off
    Still 'allon_noclosure'; Still 'allon_noacc'; Still 'allon_nolayers'; Still 'allon_passes1'
  }
  'recheck' { Still 'allon' 'allon_sym'; if ($res -eq '1920x1080') { Rot 'allon' 'allon_sym' } }
  'lobby' {
    # the coordinator's noisiest game scene (568 shadowed lights in view): PG_MODES with the game's exposure; final, GI and
    # reflection layers at frames 1/4/16/299, and the rotation (PG_ROT = 1)
    foreach ($m in ($env:PG_MODES -split ',')) {
      Step "still_${k}_${res}_$m" 'unx_gate_shadow_renderergate.exe' (@('--scene', $scenes[$k], '--resolution', $res, '--warmup-frames', '0', '--frames', '300', '--capture-frames', '1,4,16,299', '--capture-layers', 'final,gi,refl', '--auto-exposure', '--capture', "$p2\still_${k}_${res}_$m.pfm") + $modes[$m])
    }
    if ($env:PG_ROT -eq '1') {
      foreach ($m in ($env:PG_MODES -split ',')) {
        Step "rot_${k}_$m" 'unx_gate_shadow_renderergate.exe' (@('--scene', $scenes[$k], '--resolution', '1920x1080', '--warmup-frames', '0', '--frames', '180', '--path-rotate', '90', '--motion-start', '60', '--capture-frames', '59,63,120,179', '--capture-layers', 'final', '--auto-exposure', '--capture', "$p2\rot_${k}_$m.pfm") + $modes[$m])
      }
    }
  }
  'show' { Still 'allon'; Still 'alloff'; if ($res -eq '1920x1080') { Rot 'allon'; Rot 'alloff' } }
  'filter' { Still 'full'; Still 'none'; Still 'passes2'; Still 'wideonly'; if ($res -eq '1920x1080') { Rot 'full'; Rot 'none' } }
  'cold' { Still 'coldoff'; Still 'visonly'; Still 'closureonly'; Still 'full' 'full_b'; Still 'coldoff' 'coldoff_b' }
  'scene' { Still 'full'; Still 'none'; if ($res -eq '1920x1080') { Rot 'full'; Rot 'none' } }
  'timing' {
    $out = 'C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\p2time'
    New-Item -ItemType Directory -Force $out | Out-Null
    foreach ($m in 'full', 'none', 'acc') { Step "time_${k}_${res}_$m" 'unx_gate_shadow_renderergate.exe' (@('--scene', $scenes[$k], '--resolution', $res, '--warmup-frames', '300', '--frames', '300', '--out', "$out\${k}_${res}_$m") + $modes[$m]) }
  }
  default { "unknown turn '$turn'" }
}
