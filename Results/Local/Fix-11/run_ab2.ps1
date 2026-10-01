$ErrorActionPreference = "Continue"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11"
Set-Location "C:\Users\USER\UnravelNext-fix"
& "build\all\bin\unx_gate_shadow_renderergate.exe" --scene "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lounge_reference.unxscene" --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 300 --capture (Join-Path $out "lounge_1080_new.pfm") *> (Join-Path $out "lounge_1080_new.log")
"lounge_1080_new exit $LASTEXITCODE"
