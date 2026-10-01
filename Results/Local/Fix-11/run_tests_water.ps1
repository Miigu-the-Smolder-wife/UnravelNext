# After-game chain (A): the basin statistics API (queue 10, INTERFACES v1.90): PoolTests (8: GPU statistics = CPU) and
# HostPools (3b: UnxPoolStatsLatest through the host). One lock hold.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($t in @("unx_test_water_pooltests", "unx_test_host_hostpools")) {
  $exe = "build\all\bin\$t.exe"
  if (-not (Test-Path $exe)) { "$t : missing"; continue }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $exe *> (Join-Path $out "$t.log")
  "$t exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
