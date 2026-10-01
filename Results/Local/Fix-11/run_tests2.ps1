# After-game chain (A): the four renderer test suites of the fix branch's current build (queue 11 + L2/L2b/L2c/L3/L4
# code-only items): FroxelTests (capacity bound, forced fallback, omission A/B, classification A/B), LocalShadowTests
# (classification lit / twin umbra, overflow prefix-sum allocation), ShadingTests (emissive panel, tile lights,
# coverage tile lights), VsmTests. One lock hold (about 4 min measured for the earlier set).
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\postgame"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($t in @("unx_test_shadow_froxeltests", "unx_test_shadow_localshadowtests", "unx_test_shading_shadingtests", "unx_test_shadow_vsmtests")) {
  $exe = "build\all\bin\$t.exe"
  if (-not (Test-Path $exe)) { "$t : missing"; continue }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $exe *> (Join-Path $out "$t.log")
  "$t exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
