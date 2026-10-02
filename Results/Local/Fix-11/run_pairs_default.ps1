# surface_cache.direct_pairs as the default: the lobby, 8 frames, reflection.lumen + surface_cache.enabled and no
# direct_pairs override - the pass CSV must show r.sc.pairs.* and no r.sc.cells. Then the refusal of a mode the pairs do not
# hold (direct_stochastic with the default direct_pairs): the gate must stop with the settings message, before any GPU work.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$lobby = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
$log = Join-Path $out "pairs_default.log"
& $exe --scene $lobby --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 --set reflection.lumen=true --set surface_cache.enabled=true --out (Join-Path $out "pairs_default_csv") *> $log
"pairs_default exit $LASTEXITCODE"
$bad = Select-String -Path $log -Pattern "DEVICE_REMOVED|removed|hung|DXGI_ERROR|0x887A" | Select-Object -First 5
if ($LASTEXITCODE -ne 0 -or $bad) { "  STOP"; $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 20; exit 1 }
$log = Join-Path $out "pairs_refusal.log"
& $exe --scene $lobby --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 2 --set reflection.lumen=true --set surface_cache.enabled=true --set surface_cache.direct_stochastic=true *> $log
"pairs_refusal exit $LASTEXITCODE (expected: not 0)"
Select-String -Path $log -Pattern "direct_pairs" | Select-Object -First 2 | ForEach-Object { "  " + $_.Line }
