# MegaLights verdict data (coordinator 2026-10-02 07:30, item 4): the first frames after a cut, mega_lights off and on,
# on the game path (the output image after the temporal upscale: --capture-output) with gi.deterministic=true.
# Lobby and train lounge, 1080p output, a static camera: the run's start is the cut. Captures f1, f4, f16 and f299.
# One set per run (about 1 min each).
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
$scenes = [ordered]@{
  lobby = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lobby.unxscene"
  train = "C:\Users\USER\UnravelGames\TrainExorcist\Artifacts\Look\lounge_reference.unxscene"
}
$sets = [ordered]@{
  off = @("--set", "gi.deterministic=true")
  ml  = @("--set", "gi.deterministic=true", "--set", "shading.mega_lights=true")
}
foreach ($scene in $scenes.Keys) {
    foreach ($set in $sets.Keys) {
        $name = "cut_${scene}_$set"
        $log = Join-Path $out "$name.log"
        $sw = $sets[$set]
        & $exe --scene $scenes[$scene] --resolution 1920x1080 --auto-exposure --warmup-frames 0 --frames 300 --capture-output (Join-Path $out "$name.pfm") --capture-frames 1,4,16,299 @sw *> $log
        $code = $LASTEXITCODE
        "$name exit $code"
        $bad = Select-String -Path $log -Pattern "DEVICE_REMOVED|removed|hung|DXGI_ERROR|0x887A" | Select-Object -First 5
        if ($code -ne 0 -or $bad) { "  STOP"; $bad | ForEach-Object { "  " + $_.Line }; Get-Content $log -Tail 20; exit 1 }
    }
}
