# Exactness check of the list plumbing (queue 11): a SceneGen scene whose froxel lists never exceeded the old fixed
# 32 entries (no truncation in the baseline log) must render identically in the baseline (338cadd) and the new build.
$ErrorActionPreference = "Continue"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11"
$builds = @{
  base = @{ root = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\baseline"; exe = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\baseline\bin\unx_gate_shadow_renderergate.exe" }
  new  = @{ root = "C:\Users\USER\UnravelNext-fix"; exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe" }
}
foreach ($b in @("base", "new")) {
  Set-Location $builds[$b].root
  $name = "city_1080_$b"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $builds[$b].exe --scene city_block --resolution 1920x1080 --warmup-frames 0 --frames 60 --capture (Join-Path $out "$name.pfm") *> (Join-Path $out "$name.log")
  "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
