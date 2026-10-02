# Sixth locked run: the first run of the particle media on shading.mega_lights' sampled volumes (VolumeSetup.hlsl; S
# records the sampled local light before the media). Coordinator's order (2026-10-02 02:15): an 8-frame smoke first,
# then the longer runs; no lounge scene, surface_cache.enabled off; stop at the first failure.
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
$log = Join-Path $out "lumen6_smoke.log"
& "$bin\unx_gate_shadow_renderergate.exe" --scene $lobby --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 @sets *> $log
Check "lumen6_smoke" $log $LASTEXITCODE
$log = Join-Path $out "lumen6_hostvfx.log"
& "$bin\unx_test_host_hostvfxmegalights.exe" --out (Join-Path $out "lumen6_hostvfx") *> $log
Check "lumen6_hostvfx" $log $LASTEXITCODE
$log = Join-Path $out "lumen6_lobby.log"
& "$bin\unx_gate_shadow_renderergate.exe" --scene $lobby --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 60 --capture (Join-Path $out "lumen6_lobby.pfm") --capture-frames 59 @sets --out (Join-Path $out "lumen6_csv") *> $log
Check "lumen6_lobby" $log $LASTEXITCODE
