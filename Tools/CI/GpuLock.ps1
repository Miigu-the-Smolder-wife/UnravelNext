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
#
# Contention (v1.39): while the command runs, a sampler thread reads every process's GPU engine busy time (3D, compute
# and copy engines; Windows' per-process "GPU Engine\Running Time" counters, the source Task Manager uses) once a second
# and keeps every process outside the command's own tree. A process whose busiest engine is busy for >= 50 ms of a 1 s
# sample (5 %; the desktop's background UI measured 6-22 ms/s) contends with the measurement: the release line says
# "contended: name (pid) N s >= 50 ms/s, peak P ms/s", and the live summary .gpulock/contention.<pid>.json (path in
# UNX_GPU_CONTENTION for the command) lets Harness::run put the samples inside its measurement window into its result
# JSON ("gpu_contention"), so a contaminated timing is marked where it is judged. The sampler runs in this wrapper,
# outside the measured process (a sample costs 1-2 ms of CPU; no GPU work).
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

    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    static extern uint PdhOpenQuery(string source, IntPtr user, out IntPtr query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    static extern uint PdhAddEnglishCounter(IntPtr query, string path, IntPtr user, out IntPtr counter);
    [DllImport("pdh.dll")]
    static extern uint PdhCollectQueryData(IntPtr query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    static extern uint PdhGetRawCounterArray(IntPtr counter, ref uint bufferSize, out uint itemCount, IntPtr buffer);
    [DllImport("pdh.dll")]
    static extern uint PdhCloseQuery(IntPtr query);

    const uint WAIT_TIMEOUT = 0x102;
    const uint KILL_ON_JOB_CLOSE = 0x2000, BREAKAWAY_OK = 0x800;

    public class Result
    {
        public int ExitCode;
        public bool TimedOut;
        public string Leftover = "";   // descendants still running after the command exited (ended)
        public int Survivors;          // processes that did not end after termination
        public double ContendedSeconds;  // seconds in which some process outside the tree kept the GPU busy >= the threshold
        public string Contention = "";   // "name (pid) N s >= T ms/s, peak P ms/s; ..." for those processes
    }

    // GPU engine busy time per process outside the job (the command's tree), sampled every IntervalMs.
    class ContentionSampler
    {
        public const int IntervalMs = 1000;
        public const double ThresholdMsPerS = 50;
        class Other { public string Name; public double BusyMs, PeakMsPerS, SecondsOver; }
        readonly IntPtr job;
        readonly string path;
        IntPtr query, counter;
        Dictionary<string, long> last = new Dictionary<string, long>();
        Stopwatch clock = Stopwatch.StartNew();
        double lastMs;
        int samples;
        double contendedSeconds;
        Dictionary<int, Other> others = new Dictionary<int, Other>();
        List<string> over = new List<string>();
        System.Threading.Thread thread;
        System.Threading.ManualResetEvent stop = new System.Threading.ManualResetEvent(false);

        public ContentionSampler(IntPtr job, string path)
        {
            this.job = job;
            this.path = path;
            if (PdhOpenQuery(null, IntPtr.Zero, out query) != 0) { query = IntPtr.Zero; return; }
            if (PdhAddEnglishCounter(query, @"\GPU Engine(*)\Running Time", IntPtr.Zero, out counter) != 0) { PdhCloseQuery(query); query = IntPtr.Zero; return; }
            Read();  // baseline
            lastMs = clock.Elapsed.TotalMilliseconds;
            thread = new System.Threading.Thread(Loop);
            thread.IsBackground = true;
            thread.Start();
        }

        // Instance name -> cumulative running time (100 ns units) of the 3D, compute and copy engines.
        Dictionary<string, long> Read()
        {
            Dictionary<string, long> values = new Dictionary<string, long>();
            PdhCollectQueryData(query);
            uint size = 0, n = 0;
            PdhGetRawCounterArray(counter, ref size, out n, IntPtr.Zero);
            if (size == 0) return values;
            IntPtr buffer = Marshal.AllocHGlobal((int)size);
            try
            {
                if (PdhGetRawCounterArray(counter, ref size, out n, buffer) != 0) return values;
                int stride = IntPtr.Size + 40;  // PDH_RAW_COUNTER_ITEM_W: name pointer + PDH_RAW_COUNTER (FirstValue at +16)
                for (int i = 0; i < (int)n; ++i)
                {
                    IntPtr item = buffer + i * stride;
                    string name = Marshal.PtrToStringUni(Marshal.ReadIntPtr(item));
                    string lower = name == null ? "" : name.ToLowerInvariant();
                    if (!(lower.EndsWith("engtype_3d") || lower.Contains("engtype_compute") || lower.Contains("engtype_copy"))) continue;
                    values[name] = Marshal.ReadInt64(item + IntPtr.Size + 16);
                }
            }
            finally { Marshal.FreeHGlobal(buffer); }
            return values;
        }

        HashSet<int> JobPids()
        {
            HashSet<int> pids = new HashSet<int>();
            const int capacity = 256;
            IntPtr buffer = Marshal.AllocHGlobal(8 + 8 * capacity);
            try
            {
                if (QueryInformationJobObject(job, 3, buffer, 8 + 8 * capacity, IntPtr.Zero))
                {
                    int count = Marshal.ReadInt32(buffer, 4);
                    for (int k = 0; k < count; ++k) pids.Add((int)Marshal.ReadInt64(buffer, 8 + 8 * k));
                }
            }
            finally { Marshal.FreeHGlobal(buffer); }
            return pids;
        }

        void Sample()
        {
            Dictionary<string, long> now = Read();
            double ms = clock.Elapsed.TotalMilliseconds, dt = Math.Max(ms - lastMs, 1.0);  // interval (ms)
            lastMs = ms;
            HashSet<int> mine = JobPids();
            Dictionary<int, double> busy = new Dictionary<int, double>();
            foreach (KeyValuePair<string, long> kv in now)
            {
                long before;
                if (!last.TryGetValue(kv.Key, out before) || kv.Value <= before) continue;
                int a = kv.Key.IndexOf("pid_"), b = a >= 0 ? kv.Key.IndexOf('_', a + 4) : -1;
                int pid;
                if (a < 0 || b < 0 || !int.TryParse(kv.Key.Substring(a + 4, b - a - 4), out pid) || pid == 0 || mine.Contains(pid)) continue;
                // The process's busiest engine over the interval (an engine cannot be busier than the interval).
                double v;
                busy.TryGetValue(pid, out v);
                busy[pid] = Math.Max(v, Math.Min((kv.Value - before) / 10000.0, dt));
            }
            last = now;
            ++samples;
            bool contended = false;
            long unixMs = (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalMilliseconds;
            foreach (KeyValuePair<int, double> kv in busy)
            {
                Other o;
                if (!others.TryGetValue(kv.Key, out o))
                {
                    o = new Other();
                    o.Name = "pid " + kv.Key;
                    try { o.Name = Process.GetProcessById(kv.Key).ProcessName; } catch (Exception) { }
                    others[kv.Key] = o;
                }
                double perS = kv.Value * 1000.0 / dt;
                o.BusyMs += kv.Value;
                o.PeakMsPerS = Math.Max(o.PeakMsPerS, perS);
                if (perS >= ThresholdMsPerS && kv.Key != 4)  // pid 4 (System): residency paging, which the measured process itself causes
                {
                    o.SecondsOver += dt / 1000.0;
                    contended = true;
                    over.Add(string.Format(System.Globalization.CultureInfo.InvariantCulture,
                        "  {{\"t_ms\": {0}, \"pid\": {1}, \"ms_per_s\": {2:F1}, \"name\": \"{3}\"}}", unixMs, kv.Key, perS, Escape(o.Name)));
                }
            }
            if (contended) contendedSeconds += dt / 1000.0;
            Write();
        }

        static string Escape(string s) { return s.Replace("\\", "\\\\").Replace("\"", "\\\""); }

        // Live summary: one "over" sample per line (Harness::run keeps those inside its measurement window).
        void Write()
        {
            if (string.IsNullOrEmpty(path)) return;
            StringBuilder b = new StringBuilder();
            b.AppendFormat(System.Globalization.CultureInfo.InvariantCulture, "{{\"interval_ms\": {0}, \"threshold_ms_per_s\": {1}, \"samples\": {2}, \"contended_seconds\": {3:F1},\n",
                           IntervalMs, ThresholdMsPerS, samples, contendedSeconds);
            b.Append("\"over\": [\n").Append(string.Join(",\n", over.ToArray())).Append("\n],\n\"others\": [\n");
            List<string> rows = new List<string>();
            foreach (KeyValuePair<int, Other> kv in others)
                if (kv.Value.BusyMs >= 10)
                    rows.Add(string.Format(System.Globalization.CultureInfo.InvariantCulture,
                        "  {{\"pid\": {0}, \"name\": \"{1}\", \"busy_ms\": {2:F1}, \"peak_ms_per_s\": {3:F1}, \"seconds_over\": {4:F1} }}", kv.Key, Escape(kv.Value.Name),
                        kv.Value.BusyMs, kv.Value.PeakMsPerS, kv.Value.SecondsOver));
            b.Append(string.Join(",\n", rows.ToArray())).Append("\n]}\n");
            string tmp = path + ".tmp";
            try
            {
                System.IO.File.WriteAllText(tmp, b.ToString(), new UTF8Encoding(false));
                if (!ReplaceFile(tmp, path)) System.IO.File.Delete(tmp);
            }
            catch (Exception) { }
        }

        void Loop()
        {
            while (!stop.WaitOne(IntervalMs))
            {
                try { Sample(); } catch (Exception) { }
            }
        }

        public void Finish(Result r)
        {
            if (query == IntPtr.Zero) return;
            stop.Set();
            thread.Join();
            try { Sample(); } catch (Exception) { }  // the last partial interval
            PdhCloseQuery(query);
            r.ContendedSeconds = contendedSeconds;
            List<string> parts = new List<string>();
            foreach (KeyValuePair<int, Other> kv in others)
                if (kv.Value.SecondsOver > 0)
                    parts.Add(string.Format(System.Globalization.CultureInfo.InvariantCulture, "{0} ({1}) {2:F0} s >= {3:F0} ms/s, peak {4:F0} ms/s", kv.Value.Name, kv.Key,
                                            kv.Value.SecondsOver, ThresholdMsPerS, kv.Value.PeakMsPerS));
            r.Contention = string.Join("; ", parts.ToArray());
        }
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
    public static Result Run(string application, string commandLine, string directory, double timeoutMinutes, string contentionPath)
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
                ContentionSampler sampler = new ContentionSampler(job, contentionPath);

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
                sampler.Finish(r);
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
  $contention = Join-Path $lockDir ("contention.{0}.json" -f $PID)
  $env:UNX_GPU_CONTENTION = $contention
  $r = [UnxGpuLockJob]::Run($launch[0], $launch[1], (Get-Location).ProviderPath, $TimeoutMinutes, $contention)
  $code = $r.ExitCode
  # Exit 87 = the device was removed (TDR) in the run, 88 = a fence wait passed its limit (D3D12.h): say so in the log.
  $tag = ""
  if ($r.TimedOut) { $tag = " TIMEOUT after $TimeoutMinutes min (process tree ended)" }
  elseif ($code -eq 87) { $tag = " DEVICE_REMOVED" }
  elseif ($code -eq 88) { $tag = " FENCE_TIMEOUT" }
  if ($r.Leftover) { $tag += " (ended leftover descendants: $($r.Leftover))" }
  if ($r.Survivors -gt 0) { $tag += " ($($r.Survivors) processes did not end)" }
  if ($r.ContendedSeconds -gt 0) { $tag += " contended: $($r.Contention)" }
  Add-History ("{0} release {1} ({2}) exit {3}{4}" -f (Get-Date).ToString("s"), $Track, $Kind, $code, $tag)
  if ($tag) { Write-Host "GpuLock.ps1:$tag" }
} finally {
  Remove-Item Env:\UNX_GPU_LOCK -ErrorAction SilentlyContinue
  Remove-Item Env:\UNX_GPU_CONTENTION -ErrorAction SilentlyContinue
  if ($contention) { Remove-Item $contention, "$contention.tmp" -ErrorAction SilentlyContinue }
  if ($acquired) {
    Remove-Item $current -ErrorAction SilentlyContinue
    $mutex.ReleaseMutex()
  }
  $mutex.Dispose()
}
exit $code
