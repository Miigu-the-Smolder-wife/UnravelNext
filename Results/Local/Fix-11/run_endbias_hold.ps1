# First run of the per-light shadow-ray end bias (INTERFACES v1.93; scene::Light::rayEndBias). Lobby 1080p, no lounge, no
# surface cache; an 8-frame smoke first.
#   smoke   mega_lights, 8 frames (the global default is now 0.01 m)
#   d001    mega_lights, default end bias, 60 frames
#   p040    mega_lights, every light's own end bias 0.4 m (--light-ray-end-bias 0.4), global 0.01
#   g040    mega_lights, global end bias 0.4 m, no light with its own
# p040 and g040 trace the same rays: their images should agree (up to run-to-run variation); d001 should not.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$bin = "C:\Users\USER\UnravelNext-fix\build\all\bin"
$lobby = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
function Check([string]$name, [string]$log, [int]$code) {
    "$name exit $code"
    $bad = Select-String -Path $log -Pattern "DEVICE_REMOVED|removed|hung|DXGI_ERROR|D3D12 ERROR|0x887A" | Select-Object -First 5
    if ($code -ne 0 -or $bad) { "  STOP"; $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 30; exit 1 }
}
$log = Join-Path $out "endbias_smoke.log"
& "$bin\unx_gate_shadow_renderergate.exe" --scene $lobby --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 --set shading.mega_lights=true *> $log
Check "endbias_smoke" $log $LASTEXITCODE
# (unx_test_host_hostabi is not part of this hold: on 2026-10-02 it stops at "S VSM: sun level 0 can reach 4089109318
# cluster entries" - GPU instance capacity x its largest mesh, unrelated to lights. Its scene commit, after every
# UnxSceneAddLight with version 1, succeeded: Results/Local/Fix-11/postgame/endbias_hostabi.log.)
$runs = [ordered]@{
  d001 = @("--set", "shading.mega_lights=true")
  p040 = @("--set", "shading.mega_lights=true", "--light-ray-end-bias", "0.4")
  g040 = @("--set", "shading.mega_lights=true", "--set", "shading.mega_lights_ray_end_bias_m=0.4")
}
foreach ($name in $runs.Keys) {
    $log = Join-Path $out "endbias_$name.log"
    $sets = $runs[$name]
    & "$bin\unx_gate_shadow_renderergate.exe" --scene $lobby --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 60 --capture (Join-Path $out "endbias_$name.pfm") --capture-frames 59 @sets *> $log
    Check "endbias_$name" $log $LASTEXITCODE
}
