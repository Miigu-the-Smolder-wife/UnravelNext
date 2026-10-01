# After-game chain (A): the bath hall under motion and a cut (unravel-motion-quality-requirement: frames 1, 2, 4 and 16
# after a cut and while turning), switches off and all on, new build, 1080p: yaw 45 deg/s from frame 0, a camera cut at
# frame 150, captures at frames 151, 152, 154, 166 (1, 2, 4, 16 after the cut) and 299. One lock hold (two runs).
param([string]$Sets = "off,all", [string]$Res = "1920x1080", [string]$Tag = "1080")
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
New-Item -ItemType Directory -Force $out | Out-Null
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene"
$all = @("--set", "shadow.vsm.classification_pages=true", "--set", "shadow.vsm.classification_twin=true", "--set", "shading.tile_lights=true",
         "--set", "shading.coverage_tile_lights=true", "--set", "shading.emissive_area_lights=true", "--set", "atmosphere.froxels.walk_omission=true")
$switches = @{ off = @(); all = $all }
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
foreach ($s in $Sets.Split(",")) {
  $name = "bath_motion_$($Tag)_$s"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $exe --scene $scene --resolution $Res --auto-exposure --warmup-frames 0 --frames 300 --path-rotate 45 --cut-at 150 --capture (Join-Path $out "$name.pfm") --capture-frames 151,152,154,166,299 --frame-log (Join-Path $out "$name.csv") @($switches[$s]) *> (Join-Path $out "$name.log")
  "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
