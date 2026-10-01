# Timing of the froxel list change (queue 11): baseline 338cadd against the new build, bath hall (146 lights), one
# resolution per lock hold (-Res 1920x1080 | 2560x1440), 150 frames after 30 warm-up frames, auto exposure, no capture.
param([string]$Res = "1920x1080", [string]$Tag = "1080")
$ErrorActionPreference = "Continue"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11"
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene"
$builds = @{
  base = @{ root = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\baseline"; exe = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\baseline\bin\unx_gate_shadow_renderergate.exe" }
  new  = @{ root = "C:\Users\USER\UnravelNext-fix"; exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe" }
}
foreach ($b in @("base", "new")) {
  Set-Location $builds[$b].root
  $name = "timing_bath_$($Tag)_$b"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $builds[$b].exe --scene $scene --resolution $Res --auto-exposure --warmup-frames 30 --frames 150 *> (Join-Path $out "$name.log")
  "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
