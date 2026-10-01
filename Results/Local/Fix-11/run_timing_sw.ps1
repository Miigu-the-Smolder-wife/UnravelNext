# After-game chain (A): timing of the V2.5 switches, new build, bath hall (146 lights): every switch off against each
# switch alone and all on, one resolution per lock hold (-Res 1920x1080 | 2560x1440), 150 frames after 30 warm-up
# frames, auto exposure, no capture. Seven runs of about 10 s each.
param([string]$Res = "1920x1080", [string]$Tag = "1080")
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
New-Item -ItemType Directory -Force $out | Out-Null
$scene = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene"
$switches = [ordered]@{
  off   = @()
  cls   = @("--set", "shadow.vsm.classification_pages=true", "--set", "shadow.vsm.classification_twin=true")
  tile  = @("--set", "shading.tile_lights=true")
  covtl = @("--set", "shading.coverage_tile_lights=true")
  emis  = @("--set", "shading.emissive_area_lights=true")
  omit  = @("--set", "atmosphere.froxels.walk_omission=true")
}
$switches["all"] = $switches.cls + $switches.tile + $switches.covtl + $switches.emis + $switches.omit
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
foreach ($s in $switches.Keys) {
  $name = "timing_sw_$($Tag)_$s"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $exe --scene $scene --resolution $Res --auto-exposure --warmup-frames 30 --frames 150 @($switches[$s]) *> (Join-Path $out "$name.log")
  "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
