param(
  [string]$Tag = "run",
  [Parameter(ValueFromRemainingArguments = $true)] [string[]]$BenchArgs = @()
)
# Runs the microbench with a background GPU clock/power sampler (nvidia-smi every 250 ms) and
# prints the clock statistics observed during the run next to the benchmark output.
# Usage: powershell -File Run.ps1 -Tag rays --only-rays
#        powershell -File Run.ps1 -Tag base --skip-raster --skip-rays --skip-pso --skip-async
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "build\Microbench.exe"
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$clk = Join-Path $root "Results\clocks_${Tag}_$stamp.csv"
$job = Start-Job -ScriptBlock {
  param($file)
  "timestamp,sm_mhz,mem_mhz,power_w,temp_c,util_pct,throttle" | Out-File -FilePath $file -Encoding ascii
  while ($true) {
    $line = & "C:\Windows\System32\nvidia-smi.exe" --query-gpu=timestamp,clocks.sm,clocks.mem,power.draw,temperature.gpu,utilization.gpu,clocks_throttle_reasons.active --format=csv,noheader,nounits
    $line | Out-File -FilePath $file -Encoding ascii -Append
    Start-Sleep -Milliseconds 250
  }
} -ArgumentList $clk
try {
  Write-Host "Microbench args: $($BenchArgs -join ' ')"
  & $exe @BenchArgs
} finally {
  Stop-Job $job | Out-Null; Remove-Job $job | Out-Null
}
$rows = Import-Csv $clk
if ($rows.Count -gt 0) {
  $sm = $rows | ForEach-Object { [int]$_.sm_mhz } | Where-Object { $_ -gt 0 }
  $pw = $rows | ForEach-Object { [double]$_.power_w }
  $sorted = $sm | Sort-Object
  "GPU clock during run: samples=$($sm.Count) sm_min=$($sorted[0]) sm_median=$($sorted[[int]($sorted.Count/2)]) sm_max=$($sorted[-1]) MHz, power_max=$(($pw | Measure-Object -Maximum).Maximum) W"
  $rows | Group-Object throttle | ForEach-Object { "  throttle reason $($_.Name): $($_.Count) samples" }
}
