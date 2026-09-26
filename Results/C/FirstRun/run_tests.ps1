$ErrorActionPreference = 'Continue'
$bin = 'C:\Users\USER\UnravelNext-Cint\build\C\bin'
$out = 'C:\Users\USER\UnravelNext-Cint\Results\C\FirstRun'
$tests = @(
    'unx_test_visibility_instancehierarchytests',
    'unx_test_visibility_morphtests',
    'unx_test_visibility_originrebasetests',
    'unx_test_visibility_runtimepooltests',
    'unx_test_host_hostruntime',
    'unx_test_host_hostterrain',
    'unx_test_visibility_visibilitytests',
    'unx_test_visibility_fragmenttests',
    'unx_test_host_hostabi'
)
foreach ($t in $tests) {
    $log = Join-Path $out "$t.log"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & (Join-Path $bin "$t.exe") *> $log
    $code = $LASTEXITCODE
    $sw.Stop()
    $hung = Select-String -Path $log -Pattern 'DEVICE_HUNG|DEVICE_REMOVED|887A0005|887A0006|887A0007|887A0001' -Quiet
    "{0,-48} exit {1,3}  {2,6:N1} s  {3}" -f $t, $code, $sw.Elapsed.TotalSeconds, $(if ($hung) { 'DEVICE REMOVED' } else { '' }) | Tee-Object -FilePath (Join-Path $out 'summary.txt') -Append
    if ($hung) { 'stopping: device removed' | Tee-Object -FilePath (Join-Path $out 'summary.txt') -Append; break }
}
