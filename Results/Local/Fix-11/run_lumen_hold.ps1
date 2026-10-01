# First run of A's Lumen modules inside the lock (lumen.short_range_ao, lumen.radiance_cache): lobby 1080p.
#   1. 8 frames with both on (stops on failure or device removal)
#   2. 300 frames with both and shading.mega_lights on, captures of frames 1 and 299
#   3. 120 frames with gi.lumen and the surface cache on as well (the probe rays' surface-cache hit path)
# Neither module has a reader yet (R's integration reads ViewResources::shortRangeAO and FrameResources::lumenRc*), so
# this checks that the passes are built, run and leave the device alive; the image is the same as without them.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
$sets = @("--set", "lumen.short_range_ao=true", "--set", "lumen.radiance_cache=true")
function Check([string]$name, [string]$log, [int]$code) {
    "$name exit $code"
    $bad = Select-String -Path $log -Pattern "removed|hung|DXGI_ERROR|D3D12 ERROR" | Select-Object -First 5
    if ($code -ne 0 -or $bad) { $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 25; exit 1 }
}
$log = Join-Path $out "lobby_lumen_smoke.log"
& $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 @sets *> $log
Check "smoke" $log $LASTEXITCODE
$log = Join-Path $out "lobby_lumen.log"
& $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 300 --capture (Join-Path $out "lobby_lumen.pfm") --capture-frames 1,299 @sets --set shading.mega_lights=true *> $log
Check "lobby_lumen" $log $LASTEXITCODE
$log = Join-Path $out "lobby_lumen_sc.log"
& $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 120 --capture (Join-Path $out "lobby_lumen_sc.pfm") --capture-frames 119 @sets --set shading.mega_lights=true --set gi.lumen=true --set surface_cache.enabled=true *> $log
Check "lobby_lumen_sc" $log $LASTEXITCODE
