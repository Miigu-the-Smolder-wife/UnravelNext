# Bath hall A/B with layers (queue 11): eight consecutive frames (temporal mean separates per-frame noise from bias) and
# the gi / refl / shadow layers of the same frames, baseline 338cadd against the new build, 1080p, auto exposure.
$ErrorActionPreference = "Continue"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\layers"
New-Item -ItemType Directory -Force $out | Out-Null
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene"
$builds = @{
  base = @{ root = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\baseline"; exe = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\baseline\bin\unx_gate_shadow_renderergate.exe" }
  new  = @{ root = "C:\Users\USER\UnravelNext-fix"; exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe" }
}
foreach ($b in @("base", "new")) {
  Set-Location $builds[$b].root
  $name = "bath_$b"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $builds[$b].exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 300 --capture (Join-Path $out "$name.pfm") --capture-frames 292,293,294,295,296,297,298,299 --capture-layers final,gi,refl,shadow *> (Join-Path $out "$name.log")
  "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
