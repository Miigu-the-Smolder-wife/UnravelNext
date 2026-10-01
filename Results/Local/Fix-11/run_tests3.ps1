# After-game chain 5 (A): the suites that failed or aborted in chain 4's tests2 before the two fixes (c8a0531), and the
# water / host suites of the new items: FroxelTests (FX 1b hook), ShadingTests (14.1b upload release; emissive panel,
# tile lights), PoolTests (statistics, round tub), HostPools (statistics), HostAbi (material v6), the CPU unit tests.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($t in @("unx_test_shadow_froxeltests", "unx_test_shading_shadingtests", "unx_test_water_pooltests", "unx_test_host_hostpools", "unx_test_host_hostabi", "unx_unit_tests")) {
  $exe = "build\all\bin\$t.exe"
  if (-not (Test-Path $exe)) { "$t : missing"; continue }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $exe *> (Join-Path $out "$t.2.log")
  "$t exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
