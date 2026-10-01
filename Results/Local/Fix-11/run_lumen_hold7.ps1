# Seventh locked run: the sampled local light on views other than the main one (planar reflection views: air and lit
# particles), and the dispatch bounds of A's ray passes (m.ml.trace in row bands, s.ml.volume in slice bands, the radiance
# cache's chunks counting 3 rays a texel). Coordinator's order: 8-frame smoke, then the planar view, then the lobby.
# No lounge scene, surface_cache.enabled off; stop at the first failure.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$bin = "C:\Users\USER\UnravelNext-fix\build\all\bin"
$lobby = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
$sets = @("--set", "shading.mega_lights=true", "--set", "lumen.short_range_ao=true", "--set", "lumen.radiance_cache=true")
function Check([string]$name, [string]$log, [int]$code) {
    "$name exit $code"
    $bad = Select-String -Path $log -Pattern "DEVICE_REMOVED|removed|hung|DXGI_ERROR|D3D12 ERROR|0x887A" | Select-Object -First 5
    if ($code -ne 0 -or $bad) { "  STOP"; $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 30; exit 1 }
}
$log = Join-Path $out "lumen7_smoke.log"
& "$bin\unx_gate_shadow_renderergate.exe" --scene $lobby --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 @sets *> $log
Check "lumen7_smoke" $log $LASTEXITCODE
$log = Join-Path $out "lumen7_water.log"
& "$bin\unx_gate_water_watergate.exe" --scene interior --resolution 1440p --frames 60 --planar on --set shading.mega_lights=true --out (Join-Path $out "lumen7_water_csv") *> $log
Check "lumen7_water" $log $LASTEXITCODE
$log = Join-Path $out "lumen7_hostvfx.log"
& "$bin\unx_test_host_hostvfxmegalights.exe" *> $log
Check "lumen7_hostvfx" $log $LASTEXITCODE
$log = Join-Path $out "lumen7_lobby.log"
& "$bin\unx_gate_shadow_renderergate.exe" --scene $lobby --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 60 --capture (Join-Path $out "lumen7_lobby.pfm") --capture-frames 59 @sets --out (Join-Path $out "lumen7_csv") *> $log
Check "lumen7_lobby" $log $LASTEXITCODE
