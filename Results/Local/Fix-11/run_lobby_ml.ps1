# shading.mega_lights (MegaLights port, A 2026-10-01) on the lobby, against the default path, same build:
#   still   300 frames, captures f1, f2, f4, f16, f299 (the first frames after the start = a cut; the converged frame)
#   motion  yaw 45 deg/s from frame 0, a camera cut at frame 150, captures f151, f152, f154, f166, f299
# One set per run (about 1 min each): -Sets off,ml  -Mode still|motion  -Res 1920x1080.
#   off     defaults (the per-pixel loop with S's slots; lobby: 128 slots, 699 shadowed lights without one)
#   ml      shading.mega_lights=true
#   nogi / norefl  defaults with shading.experiment_disable 2 / 4 (the lobby defect attribution, still only)
param([string]$Sets = "off,ml", [string]$Mode = "still", [string]$Res = "1920x1080", [string]$Tag = "1080", [string]$Scene = "lobby")
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
New-Item -ItemType Directory -Force $out | Out-Null
$scenes = @{
  lobby  = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
  bath   = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene"
  lounge = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lounge_reference.unxscene"
}
$switches = @{
  off    = @()
  ml     = @("--set", "shading.mega_lights=true")
  ml_e1  = @("--set", "shading.mega_lights=true", "--set", "shading.mega_lights_ray_end_bias_m=0.01")
  ml_ns  = @("--set", "shading.mega_lights=true", "--set", "shading.mega_lights_ray_end_bias_m=1000")
  ml_b10 = @("--set", "shading.mega_lights=true", "--set", "shading.mega_lights_ray_bias_m=0.1")
  d_off  = @("--set", "shading.experiment_disable=30")
  d_ml   = @("--set", "shading.mega_lights=true", "--set", "shading.experiment_disable=30")
  d_mlns = @("--set", "shading.mega_lights=true", "--set", "shading.experiment_disable=30", "--set", "shading.mega_lights_ray_end_bias_m=1000")
  nogi   = @("--set", "shading.experiment_disable=2")
  norefl = @("--set", "shading.experiment_disable=4")
}
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
foreach ($s in $Sets.Split(",")) {
  $name = "$($Scene)_$($Tag)_$($Mode)_$s"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  if ($Mode -eq "motion") {
    & $exe --scene $scenes[$Scene] --resolution $Res --auto-exposure --warmup-frames 0 --frames 300 --path-rotate 45 --cut-at 150 --capture (Join-Path $out "$name.pfm") --capture-frames 151,152,154,166,299 --frame-log (Join-Path $out "$name.csv") @($switches[$s]) *> (Join-Path $out "$name.log")
  } else {
    & $exe --scene $scenes[$Scene] --resolution $Res --auto-exposure --warmup-frames 0 --frames 300 --capture (Join-Path $out "$name.pfm") --capture-frames 1,2,4,16,299 --frame-log (Join-Path $out "$name.csv") @($switches[$s]) *> (Join-Path $out "$name.log")
  }
  "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
  Select-String -Path (Join-Path $out "$name.log") -Pattern "device removed|device hung|DXGI_ERROR|TDR|FAIL|error" -SimpleMatch:$false | Select-Object -First 5 | ForEach-Object { "  " + $_.Line }
}
