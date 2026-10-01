# MegaLights verdict data (coordinator 2026-10-02 06:30, item 3): the lobby's ceiling cove band disappears with
# shading.mega_lights on. The band's lights are 387 rect strips 0.9 - 4.9 m x 0.04 m at y = 7.705 (range 5 m, shadows on;
# Results/Local/Fix-11/postgame/lobby_lights.txt). S's shadow maps started at max(5 cm, emitter radius = half diagonal,
# up to 2.45 m) from a light's centre; the sampled shadow ray ends mega_lights_ray_end_bias_m (5 cm) before its point on
# the light. One lock: the old path, then mega_lights with the end bias swept - the bias at which the band returns is the
# distance of the blocking geometry from the strips. 1080p, 60 frames each, fixed exposure from the scene camera.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
$runs = [ordered]@{
  off   = @()
  b005  = @("--set", "shading.mega_lights=true")
  b010  = @("--set", "shading.mega_lights=true", "--set", "shading.mega_lights_ray_end_bias_m=0.1")
  b020  = @("--set", "shading.mega_lights=true", "--set", "shading.mega_lights_ray_end_bias_m=0.2")
  b040  = @("--set", "shading.mega_lights=true", "--set", "shading.mega_lights_ray_end_bias_m=0.4")
  b080  = @("--set", "shading.mega_lights=true", "--set", "shading.mega_lights_ray_end_bias_m=0.8")
  b250  = @("--set", "shading.mega_lights=true", "--set", "shading.mega_lights_ray_end_bias_m=2.5")
}
foreach ($name in $runs.Keys) {
    $log = Join-Path $out "cove_$name.log"
    $sets = $runs[$name]
    & $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 60 --capture (Join-Path $out "cove_$name.pfm") --capture-frames 59 @sets *> $log
    $code = $LASTEXITCODE
    "cove_$name exit $code"
    $bad = Select-String -Path $log -Pattern "DEVICE_REMOVED|removed|hung|DXGI_ERROR|0x887A" | Select-Object -First 5
    if ($code -ne 0 -or $bad) { "  STOP"; $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 20; exit 1 }
}
