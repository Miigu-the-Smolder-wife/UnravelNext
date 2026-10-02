# Second locked run of A's Lumen modules and the first of "A9 area-light lobes on the light samples":
#   1. lobby 1080p, 120 frames, lumen.short_range_ao + lumen.radiance_cache + shading.mega_lights + gi.lumen + surface
#      cache, with --out: the pass timing CSV shows whether r.gi.rc.* / r.gi.sao* ran (the gate's log lists s.* only)
#   2. train lounge and bath hall, 120 frames each, the same switches (layered / sheen materials under area lights)
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$scenes = [ordered]@{
  lobby = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
  train = "C:\Users\USER\UnravelGames\TrainExorcist\Artifacts\Look\lounge_reference.unxscene"
  bath  = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene"
}
$sets = @("--set", "lumen.short_range_ao=true", "--set", "lumen.radiance_cache=true", "--set", "shading.mega_lights=true",
          "--set", "gi.lumen=true", "--set", "surface_cache.enabled=true")
foreach ($name in $scenes.Keys) {
    $log = Join-Path $out "lumen2_$name.log"
    $extra = @()
    if ($name -eq "lobby") { $extra = @("--out", (Join-Path $out "lumen2_csv")) }
    & $exe --scene $scenes[$name] --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 120 --capture (Join-Path $out "lumen2_$name.pfm") --capture-frames 119 @sets @extra *> $log
    $code = $LASTEXITCODE
    "lumen2_$name exit $code"
    $bad = Select-String -Path $log -Pattern "removed|hung|DXGI_ERROR|D3D12 ERROR" | Select-Object -First 5
    if ($code -ne 0 -or $bad) { $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 25; exit 1 }
}
