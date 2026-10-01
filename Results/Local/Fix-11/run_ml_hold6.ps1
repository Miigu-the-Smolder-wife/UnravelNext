# First run of shading.mega_lights with S's local shadow slots off (VsmSystem megaLightsOwnLocalShadows), inside the lock:
# lobby smoke (8 frames; stops on failure or device removal), then lobby, the train lounge and the bath hall 1080p still
# with the switch on, and the lobby with the switch off (the default path must still assign its slots).
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
$log = Join-Path $out "lobby_1080d_smoke_ml.log"
& $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 --capture (Join-Path $out "lobby_1080d_smoke_ml.pfm") --capture-frames 1,7 --set shading.mega_lights=true *> $log
$code = $LASTEXITCODE
"smoke exit $code"
$bad = Select-String -Path $log -Pattern "removed|hung|DXGI_ERROR" | Select-Object -First 5
if ($code -ne 0 -or $bad) { $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 25; exit 1 }
$p = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\run_lobby_ml.ps1"
& $p -Sets "ml" -Mode still -Tag 1080d -Scene lobby
& $p -Sets "ml" -Mode still -Tag 1080d -Scene train
& $p -Sets "ml" -Mode still -Tag 1080d -Scene bath
& $p -Sets "off" -Mode still -Tag 1080d -Scene lobby
