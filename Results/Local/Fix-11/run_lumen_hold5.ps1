# Fifth locked run, after the structural fixes (radiance cache dispatches in chunks; planar views without persistent
# MegaLights state): the planar path's first run without state, and the lobby with every switch on.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$bin = "C:\Users\USER\UnravelNext-fix\build\all\bin"
function Check([string]$name, [string]$log, [int]$code) {
    "$name exit $code"
    $bad = Select-String -Path $log -Pattern "DEVICE_REMOVED|removed|hung|DXGI_ERROR|D3D12 ERROR|0x887A" | Select-Object -First 5
    if ($code -ne 0 -or $bad) { $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 30; exit 1 }
}
$log = Join-Path $out "lumen5_water.log"
& "$bin\unx_gate_water_watergate.exe" --scene interior --resolution 1440p --frames 60 --planar on --set shading.mega_lights=true --out (Join-Path $out "lumen5_water_csv") *> $log
Check "lumen5_water" $log $LASTEXITCODE
$log = Join-Path $out "lumen5_lobby.log"
& "$bin\unx_gate_shadow_renderergate.exe" --scene "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene" --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 60 --capture (Join-Path $out "lumen5_lobby.pfm") --capture-frames 59 --set lumen.short_range_ao=true --set lumen.radiance_cache=true --set shading.mega_lights=true --set gi.lumen=true --set surface_cache.enabled=true --out (Join-Path $out "lumen5_csv") *> $log
Check "lumen5_lobby" $log $LASTEXITCODE
