# GPU measurement lock (INTERFACES_KO.md 3.3). Performance measurements from all sessions run one at a time:
#   powershell -File Tools/CI/GpuLock.ps1 -Track S -- build/S/bin/unx_gate_shadow_vsm.exe --resolution 4K
# Holds the named mutex "Local\UnravelNext.GpuMeasurement" while the command runs and sets UNX_GPU_LOCK=<track> for it
# (Harness::run and every gate refuse to measure without it). -Kind says what the holder does: timing (default: a
# measurement; CPU-heavy background jobs pause for it) or correctness (a new kernel's first hardware run, serialised on
# the GPU; background CPU jobs need not pause). The current holder is in .gpulock/current.json (with "kind"), the
# history in .gpulock/history.log ("acquire <track> (<kind>) :: ...").
#
# Hold limit (v1.30): the command runs in a Job object (kill on close). After -TimeoutMinutes (default 45: the longest
# legitimate hold on record is a 27.6 min Unity test run) the whole process tree is ended and the release line says
# TIMEOUT (exit 124); a killed wrapper takes its command's tree with it (the job's last handle closes), so no orphaned
# GPU process outlives the lock. Descendants still alive 5 s after the command itself exits are ended and named in the
# release line. A holder that died without a release line (abandoned mutex, or its current.json left behind with its
# process gone) is logged as "stale release" by the next acquirer. -WaitMinutes (default 120) bounds the wait for the
# lock.
# Arguments are parsed by hand (no param block) so everything after "--" reaches the command unchanged.
$ErrorActionPreference = "Stop"
$Track = $null
$WaitMinutes = 120
$TimeoutMinutes = 45
$Kind = "timing"
$Command = @()
for ($i = 0; $i -lt $args.Count; $i++) {
  $a = [string]$args[$i]
  if ($a -eq "--") {
    if ($i + 1 -lt $args.Count) { $Command = @($args[($i + 1)..($args.Count - 1)]) }
    break
  } elseif ($a -eq "-Track") {
    $Track = [string]$args[++$i]
  } elseif ($a -eq "-TimeoutMinutes") {
    $TimeoutMinutes = [double]$args[++$i]
  } elseif ($a -eq "-WaitMinutes") {
    $WaitMinutes = [double]$args[++$i]
  } elseif ($a -eq "-Kind") {
    $Kind = [string]$args[++$i]
    if ($Kind -ne "timing" -and $Kind -ne "correctness") { throw "GpuLock.ps1: -Kind timing|correctness" }
  } else {
    throw "GpuLock.ps1: unexpected argument '$a'. Usage: GpuLock.ps1 -Track <name> [-Kind timing|correctness] [-TimeoutMinutes N] [-WaitMinutes N] -- <command> [args...]"
  }
}
if (-not $Track) { throw "GpuLock.ps1: -Track <name> is required" }
if ($Command.Count -eq 0) { throw "GpuLock.ps1: no command after --" }
if (-not ($TimeoutMinutes -gt 0)) { throw "GpuLock.ps1: -TimeoutMinutes must be positive" }

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public static class UnxGpuLockJob
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO
    {
        public int cb;
        public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [StructLayout(LayoutKind.Sequential)]
    struct BASIC_LIMIT
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS { public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes; }
    [StructLayout(LayoutKind.Sequential)]
    struct EXTENDED_LIMIT
    {
        public BASIC_LIMIT Basic;
        public IO_COUNTERS Io;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct BASIC_ACCOUNTING
    {
        public long TotalUserTime, TotalKernelTime, ThisPeriodTotalUserTime, ThisPeriodTotalKernelTime;
        public uint TotalPageFaultCount, TotalProcesses, ActiveProcesses, TotalTerminatedProcesses;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref EXTENDED_LIMIT info, int length);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool QueryInformationJobObject(IntPtr job, int infoClass, out BASIC_ACCOUNTING info, int length, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool QueryInformationJobObject(IntPtr job, int infoClass, IntPtr info, int length, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TerminateJobObject(IntPtr job, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string application, StringBuilder commandLine, IntPtr processAttributes, IntPtr threadAttributes,
                                      bool inheritHandles, uint flags, IntPtr environment, string directory, ref STARTUPINFO startup,
                                      out PROCESS_INFORMATION info);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr GetStdHandle(int which);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool MoveFileExW(string from, string to, uint flags);

    const uint WAIT_TIMEOUT = 0x102;
    const uint KILL_ON_JOB_CLOSE = 0x2000, BREAKAWAY_OK = 0x800;

    public class Result
    {
        public int ExitCode;
        public bool TimedOut;
        public string Leftover = "";   // descendants still running after the command exited (ended)
        public int Survivors;          // processes that did not end after termination
    }

    // Replace 'to' with 'from' in one rename (readers open the file with delete sharing).
    public static bool ReplaceFile(string from, string to) { return MoveFileExW(from, to, 1 | 8); }

    static uint Active(IntPtr job)
    {
        BASIC_ACCOUNTING a;
        return QueryInformationJobObject(job, 1, out a, Marshal.SizeOf(typeof(BASIC_ACCOUNTING)), IntPtr.Zero) ? a.ActiveProcesses : 0;
    }

    static string Names(IntPtr job)
    {
        const int capacity = 256;
        IntPtr buffer = Marshal.AllocHGlobal(8 + 8 * capacity);
        try
        {
            if (!QueryInformationJobObject(job, 3, buffer, 8 + 8 * capacity, IntPtr.Zero)) return "?";
            int count = Marshal.ReadInt32(buffer, 4);
            List<string> names = new List<string>();
            for (int k = 0; k < count; ++k)
            {
                int id = (int)Marshal.ReadInt64(buffer, 8 + 8 * k);
                string name = "pid " + id;
                try { name = Process.GetProcessById(id).ProcessName + " (" + id + ")"; } catch (Exception) { }
                names.Add(name);
            }
            return string.Join(", ", names.ToArray());
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    static int Drain(IntPtr job, int milliseconds)
    {
        Stopwatch w = Stopwatch.StartNew();
        while (Active(job) > 0 && w.ElapsedMilliseconds < milliseconds) System.Threading.Thread.Sleep(50);
        return (int)Active(job);
    }

    // Runs 'commandLine' (application path 'application') in a new kill-on-close job with this process's standard
    // handles, and waits for it at most 'timeoutMinutes'.
    public static Result Run(string application, string commandLine, string directory, double timeoutMinutes)
    {
        IntPtr job = CreateJobObjectW(IntPtr.Zero, null);  // not inheritable: only this process holds it
        if (job == IntPtr.Zero) throw new Win32Exception();
        try
        {
            EXTENDED_LIMIT limit = new EXTENDED_LIMIT();
            // BREAKAWAY_OK: a child that asks to leave the job (a service-like helper) may; everything else stays in it.
            limit.Basic.LimitFlags = KILL_ON_JOB_CLOSE | BREAKAWAY_OK;
            if (!SetInformationJobObject(job, 9, ref limit, Marshal.SizeOf(typeof(EXTENDED_LIMIT)))) throw new Win32Exception();

            STARTUPINFO si = new STARTUPINFO();
            si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            si.dwFlags = 0x100;  // STARTF_USESTDHANDLES
            si.hStdInput = GetStdHandle(-10);
            si.hStdOutput = GetStdHandle(-11);
            si.hStdError = GetStdHandle(-12);
            foreach (IntPtr h in new IntPtr[] { si.hStdInput, si.hStdOutput, si.hStdError })
                if (h != IntPtr.Zero && h != new IntPtr(-1)) SetHandleInformation(h, 1, 1);  // HANDLE_FLAG_INHERIT

            PROCESS_INFORMATION pi;
            // Suspended until it is in the job, so no grandchild starts outside it.
            if (!CreateProcessW(application, new StringBuilder(commandLine), IntPtr.Zero, IntPtr.Zero, true, 0x4, IntPtr.Zero, directory, ref si, out pi))
                throw new Win32Exception();
            try
            {
                if (!AssignProcessToJobObject(job, pi.hProcess))
                {
                    int error = Marshal.GetLastWin32Error();
                    TerminateProcess(pi.hProcess, 1);
                    throw new Win32Exception(error);
                }
                ResumeThread(pi.hThread);

                Result r = new Result();
                double ms = timeoutMinutes * 60000.0;
                if (WaitForSingleObject(pi.hProcess, ms >= 4294967294.0 ? 4294967294u : (uint)ms) == WAIT_TIMEOUT)
                {
                    r.TimedOut = true;
                    TerminateJobObject(job, 124);
                    WaitForSingleObject(pi.hProcess, 60000);
                    r.Survivors = Drain(job, 60000);
                }
                else if (Drain(job, 5000) > 0)
                {
                    r.Leftover = Names(job);
                    TerminateJobObject(job, 124);
                    r.Survivors = Drain(job, 60000);
                }
                uint code;
                GetExitCodeProcess(pi.hProcess, out code);
                r.ExitCode = r.TimedOut ? 124 : (int)code;
                return r;
            }
            finally
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        finally { CloseHandle(job); }
    }
}
'@

# One argument quoted for a Windows command line (CommandLineToArgvW rules).
function Format-Argument([string]$a) {
  if ($a -eq "") { return '""' }
  if ($a -notmatch '[\s"]') { return $a }
  $e = $a -replace '(\\*)"', '$1$1\"'
  $e = $e -replace '(\\+)$', '$1$1'
  return '"' + $e + '"'
}

# (application path, command line) for the command: an executable directly, a .cmd/.bat through cmd.exe, a .ps1 through
# powershell.exe. GUI-subsystem executables (Unity.exe) take the same path: the job wait covers them and their
# descendants (v1.26 used Start-Process -Wait for them).
function Resolve-Launch([object[]]$cmd) {
  $name = [string]$cmd[0]
  $rest = @()
  if ($cmd.Count -gt 1) { $rest = @($cmd[1..($cmd.Count - 1)] | ForEach-Object { [string]$_ }) }
  $c = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $c) { throw "GpuLock.ps1: command '$name' not found" }
  if ($c.CommandType -eq "Application") {
    $path = $c.Source
    $ext = [IO.Path]::GetExtension($path).ToLowerInvariant()
    if ($ext -eq ".cmd" -or $ext -eq ".bat") {
      $shell = $env:ComSpec
      $inner = (@($path) + $rest | ForEach-Object { Format-Argument $_ }) -join " "
      return @($shell, ((Format-Argument $shell) + ' /d /s /c "' + $inner + '"'))
    }
    return @($path, ((@($path) + $rest | ForEach-Object { Format-Argument $_ }) -join " "))
  }
  if ($c.CommandType -eq "ExternalScript") {
    $ps = (Get-Command powershell.exe).Source
    return @($ps, ((@($ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $c.Source) + $rest | ForEach-Object { Format-Argument $_ }) -join " "))
  }
  throw "GpuLock.ps1: '$name' is a $($c.CommandType); give an executable or a script"
}

$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$lockDir = Join-Path $root ".gpulock"
New-Item -ItemType Directory -Force $lockDir | Out-Null
$current = Join-Path $lockDir "current.json"
$history = Join-Path $lockDir "history.log"
$utf8 = New-Object Text.UTF8Encoding($false)

# current.json is read by every waiter while the holder may replace it: readers share read, write and delete; the holder
# (only the mutex holder writes) writes a temporary file and renames it over.
function Read-Current {
  for ($k = 0; $k -lt 20; $k++) {
    if (-not (Test-Path $current)) { return $null }
    try {
      $fs = New-Object IO.FileStream($current, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
      try { return (New-Object IO.StreamReader($fs, $utf8)).ReadToEnd() } finally { $fs.Dispose() }
    } catch { Start-Sleep -Milliseconds 25 }
  }
  return $null
}
function Write-Current([string]$text) {
  $tmp = "$current.$PID.tmp"
  [IO.File]::WriteAllText($tmp, $text, $utf8)
  for ($k = 0; $k -lt 40; $k++) {
    if ([UnxGpuLockJob]::ReplaceFile($tmp, $current)) { return }
    Start-Sleep -Milliseconds 25
  }
  Remove-Item $tmp -ErrorAction SilentlyContinue
  Write-Warning "GpuLock.ps1: could not write $current (the lock is held regardless)"
}
function Add-History([string]$line) {
  for ($k = 0; $k -lt 40; $k++) {
    try {
      $fs = New-Object IO.FileStream($history, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]'ReadWrite, Delete')
      try {
        $bytes = $utf8.GetBytes($line + "`r`n")
        $fs.Write($bytes, 0, $bytes.Length)
        return
      } finally { $fs.Dispose() }
    } catch { Start-Sleep -Milliseconds 25 }
  }
  Write-Warning "GpuLock.ps1: could not append to $history"
}

$mutex = New-Object System.Threading.Mutex($false, "Local\UnravelNext.GpuMeasurement")
$acquired = $false
$waitStart = Get-Date
$code = 1
try {
  while (-not $acquired) {
    $abandoned = $false
    try {
      $acquired = $mutex.WaitOne([TimeSpan]::FromSeconds(10))
    } catch [System.Threading.AbandonedMutexException] {
      $acquired = $true   # the previous holder died without releasing; the lock is ours
      $abandoned = $true
    }
    if ($acquired) {
      # A holder that died without its release: the mutex comes back abandoned when someone was waiting for it, and
      # its current.json is left behind either way (a releasing holder deletes it before releasing the mutex; with no
      # other handle open, a killed holder's mutex simply disappears).
      $stale = Read-Current
      $what = $null
      if ($stale) {
        try {
          $s = $stale | ConvertFrom-Json
          $gone = -not (Get-Process -Id ([int]$s.pid) -ErrorAction SilentlyContinue)
          if ($abandoned -or $gone) { $what = "{0} ({1}) holder pid {2} gone :: {3}" -f $s.track, $s.kind, $s.pid, $s.command }
        } catch { $what = $stale.Trim() }
      } elseif ($abandoned) { $what = "(unknown holder)" }
      if ($what) { Add-History ("{0} stale release {1}" -f (Get-Date).ToString("s"), $what) }
    }
    if (-not $acquired) {
      $holder = Read-Current
      if (-not $holder) { $holder = "(unknown)" }
      Write-Host "waiting for the GPU measurement lock; held by: $holder"
      if (((Get-Date) - $waitStart).TotalMinutes -ge $WaitMinutes) { throw "GPU lock not acquired within $WaitMinutes minutes" }
    }
  }
  $launch = Resolve-Launch $Command
  $info = [ordered]@{ track = $Track; kind = $Kind; pid = $PID; started = (Get-Date).ToString("s"); timeoutMinutes = $TimeoutMinutes; command = ($Command -join " ") }
  Write-Current ($info | ConvertTo-Json -Compress)
  Add-History ("{0} acquire {1} ({2}) :: {3}" -f $info.started, $Track, $Kind, $info.command)
  $env:UNX_GPU_LOCK = $Track
  $r = [UnxGpuLockJob]::Run($launch[0], $launch[1], (Get-Location).ProviderPath, $TimeoutMinutes)
  $code = $r.ExitCode
  # Exit 87 = the device was removed (TDR) in the run, 88 = a fence wait passed its limit (D3D12.h): say so in the log.
  $tag = ""
  if ($r.TimedOut) { $tag = " TIMEOUT after $TimeoutMinutes min (process tree ended)" }
  elseif ($code -eq 87) { $tag = " DEVICE_REMOVED" }
  elseif ($code -eq 88) { $tag = " FENCE_TIMEOUT" }
  if ($r.Leftover) { $tag += " (ended leftover descendants: $($r.Leftover))" }
  if ($r.Survivors -gt 0) { $tag += " ($($r.Survivors) processes did not end)" }
  Add-History ("{0} release {1} ({2}) exit {3}{4}" -f (Get-Date).ToString("s"), $Track, $Kind, $code, $tag)
  if ($tag) { Write-Host "GpuLock.ps1:$tag" }
} finally {
  Remove-Item Env:\UNX_GPU_LOCK -ErrorAction SilentlyContinue
  if ($acquired) {
    Remove-Item $current -ErrorAction SilentlyContinue
    $mutex.ReleaseMutex()
  }
  $mutex.Dispose()
}
exit $code
