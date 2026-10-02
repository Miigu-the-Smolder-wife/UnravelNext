# Fourth locked run: the first run of the coverage layer's MegaLights instance (m.ml.cov.*; MegaLightsCoverage.hlsl, the
# FULL1 shade kernel, the demodulated spatial output, the coverage fragment kernels reading it).
# Game scenes with coverage records [measured, hold 2]: lobby 33,273 pixels, train lounge 68,147 (327 heavy tiles), bath
# hall 28,472. 1080p, 120 frames each, every switch on; pass CSV for each.
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
    $log = Join-Path $out "lumen4_$name.log"
    & $exe --scene $scenes[$name] --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 120 --capture (Join-Path $out "lumen4_$name.pfm") --capture-frames 1,119 @sets --out (Join-Path $out "lumen4_csv_$name") *> $log
    $code = $LASTEXITCODE
    "lumen4_$name exit $code"
    $bad = Select-String -Path $log -Pattern "removed|hung|DXGI_ERROR|D3D12 ERROR" | Select-Object -First 5
    if ($code -ne 0 -or $bad) { $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 30; exit 1 }
}
