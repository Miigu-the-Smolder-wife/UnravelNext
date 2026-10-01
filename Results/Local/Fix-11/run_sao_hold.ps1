# First run of lumen.short_range_ao inside the lock: lobby 1080p, 8 frames (stops on failure or device removal), then
# 300 frames with it and shading.mega_lights on. The pass has no reader yet (R's integration will read
# ViewResources::shortRangeAO), so this checks that it builds its passes and runs.
param([string]$Extra = "")
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
$sets = @("--set", "lumen.short_range_ao=true")
if ($Extra) { foreach ($e in $Extra.Split(",")) { $sets += @("--set", $e) } }
$log = Join-Path $out "lobby_lumen_smoke.log"
& $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 @sets *> $log
$code = $LASTEXITCODE
"smoke exit $code"
$bad = Select-String -Path $log -Pattern "removed|hung|DXGI_ERROR" | Select-Object -First 5
if ($code -ne 0 -or $bad) { $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 25; exit 1 }
$sw = [Diagnostics.Stopwatch]::StartNew()
& $exe --scene $scene --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 300 --capture (Join-Path $out "lobby_lumen.pfm") --capture-frames 1,299 @sets --set shading.mega_lights=true *> (Join-Path $out "lobby_lumen.log")
"lobby_lumen exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
