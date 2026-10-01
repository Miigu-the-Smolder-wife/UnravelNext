# TDR isolation (coordinator 23:45): the bath lounge died with DEVICE_HUNG on the integration branch with every switch
# on. A's switches alone, 8 frames each, stopping at the first failure (a TDR resets the user's screen: no repeats).
# After the structural fixes (probe rays, filter and store in chunks of at most 262,144 rays; planar views without
# persistent MegaLights state), in the coordinator's order:
#   1. lumen.radiance_cache alone (standalone: no gi.lumen)
#   2. shading.mega_lights alone (its volume, per-view instances, coverage instance)
#   3. both with lumen.short_range_ao
param([int]$From = 1)
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lounge_reference.unxscene"
$runs = @(
  @{ name = "rc";       sets = @("--set", "lumen.radiance_cache=true") },
  @{ name = "ml";       sets = @("--set", "shading.mega_lights=true") },
  @{ name = "ml_lumen"; sets = @("--set", "shading.mega_lights=true", "--set", "lumen.short_range_ao=true", "--set", "lumen.radiance_cache=true") }
)
for ($i = $From - 1; $i -lt $runs.Count; $i++) {
    $r = $runs[$i]
    $log = Join-Path $out ("tdr_lounge_" + $r.name + ".log")
    $sets = $r.sets
    & $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 @sets --out (Join-Path $out ("tdr_lounge_csv_" + $r.name)) *> $log
    $code = $LASTEXITCODE
    "tdr_lounge_$($r.name) exit $code"
    $bad = Select-String -Path $log -Pattern "DEVICE_REMOVED|removed|hung|DXGI_ERROR|0x887A" | Select-Object -First 5
    if ($code -ne 0 -or $bad) { "  STOP: failure with $($r.sets -join ' ')"; $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 20; exit 1 }
}
