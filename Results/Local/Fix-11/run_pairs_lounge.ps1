# surface_cache.direct_pairs in the bath lounge (the scene of the device-hung report). Approved by the user in this
# session on 2026-10-02 ("라운지 돌려"). UNX_DRED=1. 1080p, surface cache at its default size (2^22).
#   1. 8 frames: reflection.lumen + surface_cache.enabled + direct_pairs
#   2. 60 frames, the same settings
#   3. 8 frames with gi.lumen, shading.mega_lights, lumen.radiance_cache and lumen.short_range_ao as well
# Stops at the first failure (a device removal resets the user's screen: at most once, no reproduction after it).
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$env:UNX_DRED = "1"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$lounge = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lounge_reference.unxscene"
$base = @("--set", "reflection.lumen=true", "--set", "surface_cache.enabled=true", "--set", "surface_cache.direct_pairs=true")
$all = $base + @("--set", "gi.lumen=true", "--set", "shading.mega_lights=true", "--set", "lumen.radiance_cache=true", "--set", "lumen.short_range_ao=true")
$runs = @(
  @{ name = "pairs_lounge8";    frames = 8;  sets = $base },
  @{ name = "pairs_lounge60";   frames = 60; sets = $base },
  @{ name = "pairs_lounge_all"; frames = 8;  sets = $all }
)
foreach ($r in $runs) {
    $name = $r.name
    $log = Join-Path $out "$name.log"
    $sets = $r.sets
    $last = $r.frames - 1
    "$name start $(Get-Date -Format HH:mm:ss)"
    & $exe --scene $lounge --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames $r.frames --capture (Join-Path $out "$name.pfm") --capture-frames $last @sets --out (Join-Path $out "${name}_csv") *> $log
    $code = $LASTEXITCODE
    "$name exit $code $(Get-Date -Format HH:mm:ss)"
    $bad = Select-String -Path $log -Pattern "DEVICE_REMOVED|removed|hung|DXGI_ERROR|0x887A" | Select-Object -First 5
    if ($code -ne 0 -or $bad) {
        "  STOP: $name failed - no further runs"
        $bad | ForEach-Object { "  " + $_.Line }
        Select-String -Path $log -Pattern "UNX_DRED|DRED" | Select-Object -Last 60 | ForEach-Object { "  " + $_.Line }
        Get-Content $log -Tail 25
        exit 1
    }
}
