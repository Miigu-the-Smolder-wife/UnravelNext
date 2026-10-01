# A/B of the froxel light lists (queue 11, V2.5 L1): baseline 338cadd (gate build, fixed 32-entry lists) against
# redesign-v2-fix (variable-length lists). Bath hall (146 lights, 92 shadowed) and lounge, 1080p, still, 300 frames,
# the last frame captured; the renderergate log keeps the froxel statistics line.
$ErrorActionPreference = "Continue"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11"
$scenes = @{
  bath   = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene"
  lounge = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lounge_reference.unxscene"
}
$builds = @{
  base = @{ root = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\baseline"; exe = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\baseline\bin\unx_gate_shadow_renderergate.exe" }
  new  = @{ root = "C:\Users\USER\UnravelNext-fix"; exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe" }
}
foreach ($b in @("base", "new")) {
  Set-Location $builds[$b].root
  foreach ($s in @("bath", "lounge")) {
    $name = "$($s)_1080_$b"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $builds[$b].exe --scene $scenes[$s] --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 300 --capture (Join-Path $out "$name.pfm") *> (Join-Path $out "$name.log")
    "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
  }
}
