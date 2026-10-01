$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
$out = "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11"
foreach ($t in @("unx_test_shadow_froxeltests", "unx_test_shadow_localshadowtests", "unx_test_shading_shadingtests", "unx_test_shadow_vsmtests")) {
  $exe = "build\all\bin\$t.exe"
  if (-not (Test-Path $exe)) { "$t : missing"; continue }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $exe *> (Join-Path $out "$t.log")
  "$t exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
}
