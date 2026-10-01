# Before / after the ShadeOpaque split (coordinator: the work-amount rule): the pre-split build (worktree
# UnravelNext-presplit at 4b3560e) against the current build, bath hall 1080p and 1440p, 30 warm-up + 150 frames, auto
# exposure, no capture; the renderergate logs hold the per-pass medians (m.shade* / m.lit sum). One resolution per hold.
param([string]$Res = "1920x1080", [string]$Tag = "1080")
$ErrorActionPreference = "Continue"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
New-Item -ItemType Directory -Force $out | Out-Null
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene"
$builds = @{
  presplit = @{ root = "C:\Users\USER\UnravelNext-presplit"; exe = "C:\Users\USER\UnravelNext-presplit\build\all\bin\unx_gate_shadow_renderergate.exe" }
  split    = @{ root = "C:\Users\USER\UnravelNext-fix"; exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe" }
}
foreach ($b in @("presplit", "split")) {
  Set-Location $builds[$b].root
  $name = "timing_split_$($Tag)_$b"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $builds[$b].exe --scene $scene --resolution $Res --auto-exposure --warmup-frames 30 --frames 150 *> (Join-Path $out "$name.log")
  "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
