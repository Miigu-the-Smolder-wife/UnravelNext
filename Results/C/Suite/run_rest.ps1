$ErrorActionPreference = 'Continue'
$bin = 'C:\Users\USER\UnravelNext\build\C\bin'
$out = 'C:\Users\USER\UnravelNext\Results\C\Suite'
$all = Get-ChildItem $bin -Filter 'unx_test_*.exe' | Where-Object { $_.BaseName -notmatch '^unx_test_reference' } | Sort-Object Name
$part1 = @()
for ($i = 0; $i -lt $all.Count; ++$i) { if (($i % 2) -eq 1) { $part1 += $all[$i] } }
$after = $false
foreach ($t in $part1) {
    if ($t.BaseName -eq 'unx_test_host_hostdeviceremoved') { $after = $true; continue }
    if (-not $after) { continue }
    $log = Join-Path $out ($t.BaseName + '.log')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $t.FullName *> $log
    $code = $LASTEXITCODE
    $sw.Stop()
    $hung = Select-String -Path $log -Pattern 'DEVICE_HUNG|887A0005|887A0006|887A0007|887A0001' -Quiet
    $line = "{0,-48} exit {1,3}  {2,7:N1} s  {3}" -f $t.BaseName, $code, $sw.Elapsed.TotalSeconds, $(if ($hung) { 'DEVICE REMOVED' } else { '' })
    Add-Content -Path (Join-Path $out 'summary.txt') -Value $line -Encoding utf8
    if ($hung) { Add-Content -Path (Join-Path $out 'summary.txt') -Value 'stopping: device removed' -Encoding utf8; break }
}
