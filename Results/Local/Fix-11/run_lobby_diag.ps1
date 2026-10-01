# Lobby defect attribution (coordinator 19:35): the lobby at 1080p, 300 frames, auto exposure, f1 and f299 captured, with
# shading.experiment_disable off / 16384 (shadow-casting lights without a VSM slot add nothing) / 8 (no air volume
# lookup) / 32 (no local lights) / 2048 (shadow visibility reads off: every slot lit). About 3 min per run.
param([string]$Sets = "off,noslot,noair,nolocal")
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
New-Item -ItemType Directory -Force $out | Out-Null
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
$bits = @{ off = 0; noslot = 16384; noair = 8; nolocal = 32; noshadow = 2048; nogi = 2; norefl = 4 }
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
foreach ($s in $Sets.Split(",")) {
  $name = "lobby_1080_$s"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 300 --capture (Join-Path $out "$name.pfm") --capture-frames 1,299 --set "shading.experiment_disable=$($bits[$s])" *> (Join-Path $out "$name.log")
  "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
