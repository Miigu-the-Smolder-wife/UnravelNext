# surface_cache.direct_pairs, first run: the lobby (a scene without the hang), 1080p, 8 frames, reflection.lumen +
# surface_cache.enabled + direct_pairs; then the same with the inline query variant. Pass CSVs show r.sc.pairs.*.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$lobby = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
$base = @("--set", "reflection.lumen=true", "--set", "surface_cache.enabled=true", "--set", "surface_cache.direct_pairs=true")
$runs = [ordered]@{
  pairs_lobby        = $base
  pairs_lobby_inline = $base + @("--set", "surface_cache.direct_pairs_inline=true")
}
foreach ($name in $runs.Keys) {
    $log = Join-Path $out "$name.log"
    $sets = $runs[$name]
    & $exe --scene $lobby --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 8 --capture (Join-Path $out "$name.pfm") --capture-frames 7 @sets --out (Join-Path $out "${name}_csv") *> $log
    $code = $LASTEXITCODE
    "$name exit $code"
    $bad = Select-String -Path $log -Pattern "DEVICE_REMOVED|removed|hung|DXGI_ERROR|D3D12 ERROR|0x887A" | Select-Object -First 5
    if ($code -ne 0 -or $bad) { "  STOP"; $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 30; exit 1 }
}
