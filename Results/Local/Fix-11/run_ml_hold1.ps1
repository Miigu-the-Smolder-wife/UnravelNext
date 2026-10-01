# First lock hold of shading.mega_lights (A 2026-10-01), lobby 1080p, one hold (about 8 min):
#   1. smoke: 8 frames with the switch on (stops the hold when it fails or the device is removed);
#   2. still off / ml (f1, f2, f4, f16, f299);  3. motion off / ml (cut at 150: f151, f152, f154, f166, f299);
#   4. the lobby defect attribution's last two sets (GI off, reflections off; defaults otherwise).
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
New-Item -ItemType Directory -Force $out | Out-Null
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
$log = Join-Path $out "lobby_1080_smoke_ml.log"
& $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 --capture (Join-Path $out "lobby_1080_smoke_ml.pfm") --capture-frames 1,7 --set shading.mega_lights=true *> $log
$code = $LASTEXITCODE
"smoke exit $code"
$bad = Select-String -Path $log -Pattern "removed|hung|DXGI_ERROR" | Select-Object -First 5
if ($code -ne 0 -or $bad) {
  $bad | ForEach-Object { "  " + $_.Line }
  Get-Content $log -Tail 25
  exit 1
}
$p = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\run_lobby_ml.ps1"
& $p -Sets "off,ml" -Mode still
& $p -Sets "off,ml" -Mode motion
& $p -Sets "nogi,norefl" -Mode still
