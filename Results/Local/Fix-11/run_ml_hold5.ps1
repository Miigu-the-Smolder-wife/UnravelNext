# First run of the complete shading.mega_lights (tiles, sample, trace, shade, sets, temporal, spatial, volume) inside the
# lock: a lobby smoke run (8 frames; the hold stops when it fails or the device is removed), then lobby, bath hall, the
# bathhouse lounge and the train lounge 1080p still (f1, f2, f4, f16, f299) and the lobby turning with a cut, switch on.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
$log = Join-Path $out "lobby_1080c_smoke_ml.log"
& $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 --capture (Join-Path $out "lobby_1080c_smoke_ml.pfm") --capture-frames 1,7 --set shading.mega_lights=true *> $log
$code = $LASTEXITCODE
"smoke exit $code"
$bad = Select-String -Path $log -Pattern "removed|hung|DXGI_ERROR" | Select-Object -First 5
if ($code -ne 0 -or $bad) { $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 25; exit 1 }
$p = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\run_lobby_ml.ps1"
& $p -Sets "ml" -Mode still -Tag 1080c -Scene lobby
& $p -Sets "ml" -Mode still -Tag 1080c -Scene bath
& $p -Sets "ml" -Mode still -Tag 1080c -Scene lounge
& $p -Sets "ml" -Mode still -Tag 1080c -Scene train
& $p -Sets "ml" -Mode motion -Tag 1080c -Scene lobby
