# After-game chain (A): still captures of the bath hall / lounge with the V2.5 switches off and on, new build only
# (the switches default off, so "off" is the baseline of every comparison). 300 frames, auto exposure, 1080p, the last
# three frames captured (temporal mean separates per-frame noise from bias; eight were planned, cut to three: the disk
# was full on 2026-10-01 18:05). About 170 s per run: at most three runs per
# lock hold (-Sets "off,cls,tile").
#   off   : every switch off
#   cls   : L3 classification pages + exact twin (shadow.vsm.classification_pages / classification_twin)
#   tile  : L2 tile lights (shading.tile_lights)
#   covtl : L2c coverage tile lights (shading.coverage_tile_lights)
#   emis  : L2b emissive area lights (shading.emissive_area_lights) - the 14.1b "black hole at f0" check
#   omit  : L4 bounded walk omission (atmosphere.froxels.walk_omission)
#   all   : every switch on
param([string]$Scene = "bath", [string]$Sets = "off,all", [string]$Res = "1920x1080", [string]$Tag = "1080")
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
New-Item -ItemType Directory -Force $out | Out-Null
$scenes = @{
  bath   = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\bath_reference.unxscene"
  lounge = "C:\Users\USER\UnravelGames\BathhouseTycoon\Artifacts\Look\lounge_reference.unxscene"
}
$switches = @{
  off   = @()
  cls   = @("--set", "shadow.vsm.classification_pages=true", "--set", "shadow.vsm.classification_twin=true")
  tile  = @("--set", "shading.tile_lights=true")
  covtl = @("--set", "shading.coverage_tile_lights=true")
  emis  = @("--set", "shading.emissive_area_lights=true")
  omit  = @("--set", "atmosphere.froxels.walk_omission=true")
}
$switches["all"] = $switches.cls + $switches.tile + $switches.covtl + $switches.emis + $switches.omit
$exe = "C:\Users\USER\UnravelNext-fix\build\all\bin\unx_gate_shadow_renderergate.exe"
foreach ($s in $Sets.Split(",")) {
  $name = "$($Scene)_$($Tag)_$s"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $exe --scene $scenes[$Scene] --resolution $Res --auto-exposure --warmup-frames 0 --frames 300 --capture (Join-Path $out "$name.pfm") --capture-frames 297,298,299 @($switches[$s]) *> (Join-Path $out "$name.log")
  "$name exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
