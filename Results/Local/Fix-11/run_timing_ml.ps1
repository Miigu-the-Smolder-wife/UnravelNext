# Timing of shading.mega_lights against the default path (same build), one resolution per lock hold: 30 warm-up + 150
# frames, auto exposure, no capture; the gate log holds the per-pass medians (postgame/pass_times.py sums them).
param([string]$Res = "1920x1080", [string]$Tag = "1080", [string]$Scene = "lobby")
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$scenes = @{
  lobby  = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
  bath   = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene"
}
$switches = [ordered]@{ off = @(); ml = @("--set", "shading.mega_lights=true") }
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
foreach ($s in $switches.Keys) {
  $name = "timing_ml_$($Scene)_$($Tag)_$s"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $exe --scene $scenes[$Scene] --resolution $Res --auto-exposure --warmup-frames 30 --frames 150 @($switches[$s]) *> (Join-Path $out "$name.log")
  "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
