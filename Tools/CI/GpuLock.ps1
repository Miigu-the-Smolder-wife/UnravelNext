# GPU measurement lock (INTERFACES_KO.md 3.3). Performance measurements from all sessions run one at a time:
#   powershell -File Tools/CI/GpuLock.ps1 -Track S -- build/S/bin/unx_gate_shadow_vsm.exe --resolution 4K
# Holds the named mutex "Local\UnravelNext.GpuMeasurement" while the command runs and sets UNX_GPU_LOCK=<track> for it
# (Harness::run and every gate refuse to measure without it). Correctness runs (tests, validation, reference
# comparisons) never take the lock. The current holder is in .gpulock/current.json, the history in .gpulock/history.log.
# Arguments are parsed by hand (no param block) so everything after "--" reaches the command unchanged.
$ErrorActionPreference = "Stop"
$Track = $null
$TimeoutMinutes = 120
$Command = @()
for ($i = 0; $i -lt $args.Count; $i++) {
  $a = [string]$args[$i]
  if ($a -eq "--") {
    if ($i + 1 -lt $args.Count) { $Command = @($args[($i + 1)..($args.Count - 1)]) }
    break
  } elseif ($a -eq "-Track") {
    $Track = [string]$args[++$i]
  } elseif ($a -eq "-TimeoutMinutes") {
    $TimeoutMinutes = [int]$args[++$i]
  } else {
    throw "GpuLock.ps1: unexpected argument '$a'. Usage: GpuLock.ps1 -Track <name> [-TimeoutMinutes N] -- <command> [args...]"
  }
}
if (-not $Track) { throw "GpuLock.ps1: -Track <name> is required" }
if ($Command.Count -eq 0) { throw "GpuLock.ps1: no command after --" }

# The resolved path of a Windows GUI-subsystem executable (PE optional header Subsystem = 2), or $null.
function Get-GuiExecutable([string]$name) {
  $cmd = Get-Command $name -ErrorAction SilentlyContinue
  if (-not $cmd -or $cmd.CommandType -ne "Application") { return $null }
  $path = $cmd.Source
  if ([IO.Path]::GetExtension($path) -ne ".exe") { return $null }
  $fs = [IO.File]::OpenRead($path)
  try {
    $br = New-Object IO.BinaryReader($fs)
    $fs.Position = 0x3C
    $pe = $br.ReadInt32()
    $fs.Position = $pe + 24 + 68  # PE signature (4) + COFF header (20) + Subsystem offset (68, PE32 and PE32+)
    if ($br.ReadUInt16() -eq 2) { return $path }
    return $null
  } finally { $fs.Dispose() }
}

# One argument quoted for a Windows command line (CommandLineToArgvW rules).
function Format-Argument([string]$a) {
  if ($a -eq "") { return '""' }
  if ($a -notmatch '[\s"]') { return $a }
  $e = $a -replace '(\\*)"', '$1$1\"'
  $e = $e -replace '(\\+)$', '$1$1'
  return '"' + $e + '"'
}

$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$lockDir = Join-Path $root ".gpulock"
New-Item -ItemType Directory -Force $lockDir | Out-Null
$current = Join-Path $lockDir "current.json"
$history = Join-Path $lockDir "history.log"

$mutex = New-Object System.Threading.Mutex($false, "Local\UnravelNext.GpuMeasurement")
$acquired = $false
$waitStart = Get-Date
$code = 1
try {
  while (-not $acquired) {
    try {
      $acquired = $mutex.WaitOne([TimeSpan]::FromSeconds(10))
    } catch [System.Threading.AbandonedMutexException] {
      $acquired = $true   # the previous holder died; the lock is ours
    }
    if (-not $acquired) {
      $holder = if (Test-Path $current) { Get-Content $current -Raw } else { "(unknown)" }
      Write-Host "waiting for the GPU measurement lock; held by: $holder"
      if (((Get-Date) - $waitStart).TotalMinutes -ge $TimeoutMinutes) { throw "GPU lock not acquired within $TimeoutMinutes minutes" }
    }
  }
  $info = [ordered]@{ track = $Track; pid = $PID; started = (Get-Date).ToString("s"); command = ($Command -join " ") }
  ($info | ConvertTo-Json -Compress) | Set-Content -Encoding utf8 $current
  Add-Content -Encoding utf8 $history ("{0} acquire {1} :: {2}" -f $info.started, $Track, $info.command)
  $env:UNX_GPU_LOCK = $Track
  $exe = $Command[0]
  $rest = @()
  if ($Command.Count -gt 1) { $rest = @($Command[1..($Command.Count - 1)]) }
  $gui = Get-GuiExecutable $exe
  if ($gui) {
    # A GUI-subsystem program (Unity.exe) returns from '&' at once: wait for it and its descendants, or the lock would
    # be released while it still uses the GPU.
    $p = Start-Process -FilePath $gui -ArgumentList (($rest | ForEach-Object { Format-Argument ([string]$_) }) -join " ") -Wait -PassThru
    $code = $p.ExitCode
  } else {
    & $exe @rest
    $code = $LASTEXITCODE
  }
  # Exit 87 = the device was removed (TDR) inside the run (D3D12.h kDeviceRemovedExitCode): say so in the log.
  $tag = if ($code -eq 87) { " DEVICE_REMOVED" } else { "" }
  Add-Content -Encoding utf8 $history ("{0} release {1} exit {2}{3}" -f (Get-Date).ToString("s"), $Track, $code, $tag)
} finally {
  Remove-Item Env:\UNX_GPU_LOCK -ErrorAction SilentlyContinue
  if ($acquired) {
    Remove-Item $current -ErrorAction SilentlyContinue
    $mutex.ReleaseMutex()
  }
  $mutex.Dispose()
}
exit $code
