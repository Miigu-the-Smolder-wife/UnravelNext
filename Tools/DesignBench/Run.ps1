param(
  [string]$Tag = "run",
  [Parameter(ValueFromRemainingArguments = $true)] [string[]]$BenchArgs = @()
)
# Runs DesignBench.exe on the hardware GPU with the nvidia-smi clock sampler (250 ms) and prints the clock statistics.
# Hardware runs measure the GPU, so they go through Tools/CI/GpuLock.ps1 (one at a time across sessions):
#   powershell -File Tools/CI/GpuLock.ps1 -Track Design -- powershell -File Tools/DesignBench/Run.ps1 -Tag cov --only-coverage
# The WARP dry run (DesignBench.exe --warp ...) does not use the hardware GPU and is started directly.
$ErrorActionPreference = "Continue"
if (-not $env:UNX_GPU_LOCK) { throw "Run.ps1 measures the GPU: run it through Tools/CI/GpuLock.ps1 -Track <track> -- powershell -File Tools/DesignBench/Run.ps1 ..." }
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "build\DesignBench.exe"
$results = Join-Path $root "Results"
New-Item -ItemType Directory -Force $results | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$clk = Join-Path $results "clocks_${Tag}_$stamp.csv"
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
  Write-Host "DesignBench args: $($BenchArgs -join ' ')"
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
