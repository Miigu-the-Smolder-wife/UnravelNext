# Third locked run: the first runs of
#   1. lit particles on shading.mega_lights' sampled volumes (FxLayerSetup ML1; s.ml.volume's fluence and moment outputs):
#      unx_test_host_hostvfxmegalights (standalone host renderer, debug layer on)
#   2. MegaLights on a planar reflection view: the water gate's interior scene with the calm-water reflection camera forced
#   3. the lobby with every switch on (s.ml.volume with its new outputs in a game scene), pass CSV
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$bin = "C:\Users\USER\UnravelNext-fix\build\all\bin"
function Check([string]$name, [string]$log, [int]$code) {
    "$name exit $code"
    $bad = Select-String -Path $log -Pattern "removed|hung|DXGI_ERROR|D3D12 ERROR" | Select-Object -First 5
    if ($code -ne 0 -or $bad) { $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 30 }
}
$log = Join-Path $out "lumen3_hostvfx.log"
& "$bin\unx_test_host_hostvfxmegalights.exe" --out (Join-Path $out "lumen3_hostvfx") *> $log
Check "lumen3_hostvfx" $log $LASTEXITCODE
$log = Join-Path $out "lumen3_water.log"
& "$bin\unx_gate_water_watergate.exe" --scene interior --resolution 1440p --frames 120 --planar on --set shading.mega_lights=true --out (Join-Path $out "lumen3_water_csv") *> $log
Check "lumen3_water" $log $LASTEXITCODE
$log = Join-Path $out "lumen3_lobby.log"
& "$bin\unx_gate_shadow_renderergate.exe" --scene "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene" --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 120 --capture (Join-Path $out "lumen3_lobby.pfm") --capture-frames 119 --set lumen.short_range_ao=true --set lumen.radiance_cache=true --set shading.mega_lights=true --set gi.lumen=true --set surface_cache.enabled=true --out (Join-Path $out "lumen3_csv") *> $log
Check "lumen3_lobby" $log $LASTEXITCODE
