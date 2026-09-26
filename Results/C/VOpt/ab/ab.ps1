# A/B/A/B timing of the depth raster kernel (old = HEAD, new = working tree): swaps the compiled TILE1/TILE2 kernels
# and runs the V gate's VSM atlas service each time. Run under the GPU lock (Tools/CI/GpuLock.ps1 -- powershell -File ...).
param([string]$Scene = "city_block", [int]$Frames = 300, [string]$Tag = "ab")
$ErrorActionPreference = "Continue"  # the gate logs to stderr
$root = "C:\Users\USER\UnravelNext"
$bin = "$root\build\C\bin"
$dst = "$bin\shaders\Passes\Visibility"
$ab = "$root\Results\C\VOpt\ab"
foreach ($round in 1, 2) {
  foreach ($which in "old", "new") {
    Copy-Item "$ab\$which\DepthRaster.ms.TILE1.dxil" "$dst\DepthRaster.ms.TILE1.DEPTH1.dxil" -Force
    Copy-Item "$ab\$which\DepthRaster.ms.TILE2.dxil" "$dst\DepthRaster.ms.TILE2.DEPTH1.dxil" -Force
    $log = "$ab\$Tag.$Scene.$which.$round.log"
    cmd /c "`"$bin\unx_gate_visibility_visibilitygate.exe`" --scene $Scene --resolution 4K --frames $Frames --service atlas32 --service-pages ring > `"$log`" 2>&1"
    $line = Select-String -Path $log -Pattern "service svc.atlas32" | Select-Object -First 1
    $bad = Select-String -Path $log -Pattern "hung|removed|TDR" | Select-Object -First 1
    "$which $round : $($line.Line) $($bad.Line)"
  }
}
Copy-Item "$ab\new\DepthRaster.ms.TILE1.dxil" "$dst\DepthRaster.ms.TILE1.DEPTH1.dxil" -Force
Copy-Item "$ab\new\DepthRaster.ms.TILE2.dxil" "$dst\DepthRaster.ms.TILE2.DEPTH1.dxil" -Force
