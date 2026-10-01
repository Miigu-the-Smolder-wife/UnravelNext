#include "GpuSlice.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

#include <windows.h>
#include <tlhelp32.h>

#include <vector>

namespace unx::reference::gpu
{
namespace
{
std::string nowText()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
    return b;
}

std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

// Appends a line with the sharing GpuLock.ps1 uses (other sessions append concurrently); retries on contention.
void appendHistory(const std::filesystem::path& file, const std::string& line)
{
    for (int k = 0; k < 40; ++k)
    {
        HANDLE h = CreateFileW(file.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            const std::string text = line + "\r\n";
            DWORD written = 0;
            WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
            CloseHandle(h);
            return;
        }
        Sleep(25);
    }
    logf("gpu slice: could not append to %s\n", file.string().c_str());
}

std::string readShared(const std::filesystem::path& file)
{
    for (int k = 0; k < 20; ++k)
    {
        HANDLE h = CreateFileW(file.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return {};
            Sleep(25);
            continue;
        }
        std::string text;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) text.append(buf, n);
        CloseHandle(h);
        return text;
    }
    return {};
}

void writeReplace(const std::filesystem::path& file, const std::string& text)
{
    const std::filesystem::path tmp = file.string() + "." + std::to_string(GetCurrentProcessId()) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << text;
    }
    for (int k = 0; k < 40; ++k)
    {
        if (MoveFileExW(tmp.wstring().c_str(), file.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        Sleep(25);
    }
    logf("gpu slice: could not write %s\n", file.string().c_str());
}

// True when pid is an ancestor of this process (a GpuLock.ps1 wrapper around it holds the lock for all of its run).
bool isAncestor(uint32_t pid)
{
    if (pid == 0) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    std::vector<std::pair<uint32_t, uint32_t>> parentOf;  // (process, parent)
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) parentOf.push_back({ e.th32ProcessID, e.th32ParentProcessID });
    CloseHandle(snap);
    uint32_t at = GetCurrentProcessId();
    for (int depth = 0; depth < 32; ++depth)
    {
        uint32_t parent = 0;
        for (const auto& [p, q] : parentOf)
            if (p == at) parent = q;
        if (parent == 0 || parent == at) return false;
        if (parent == pid) return true;
        at = parent;
    }
    return false;
}

bool processAlive(uint32_t pid)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    CloseHandle(p);
    return alive;
}

// Value of "key" in a flat JSON object (string or number).
std::string field(const std::string& json, const char* key)
{
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = json.find(k);
    if (p == std::string::npos) return {};
    size_t q = p + k.size();
    while (q < json.size() && json[q] == ' ') ++q;
    if (q < json.size() && json[q] == '"')
    {
        const size_t e = json.find('"', q + 1);
        return json.substr(q + 1, e == std::string::npos ? std::string::npos : e - q - 1);
    }
    const size_t e = json.find_first_of(",}", q);
    return json.substr(q, e == std::string::npos ? std::string::npos : e - q);
}

// The main checkout of 'root': a worktree's .git file names its git folder, whose "commondir" leads to the shared git
// folder, whose parent is the main checkout (GpuLock.ps1 v1.83: one .gpulock for all worktrees); 'root' otherwise.
std::filesystem::path mainCheckout(const std::filesystem::path& root)
{
    namespace fsx = std::filesystem;
    std::error_code ec;
    const fsx::path dotGit = root / ".git";
    if (!fsx::is_regular_file(dotGit, ec)) return root;
    auto trimmed = [](std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return s.substr(i);
    };
    const std::string text = trimmed(readShared(dotGit));
    if (text.rfind("gitdir:", 0) != 0) return root;
    fsx::path gitDir = trimmed(text.substr(7));
    if (gitDir.is_relative()) gitDir = root / gitDir;
    fsx::path common = gitDir;
    const std::string commonDir = trimmed(readShared(gitDir / "commondir"));
    if (!commonDir.empty())
    {
        common = commonDir;
        if (common.is_relative()) common = gitDir / common;
    }
    std::wstring s = fsx::absolute(common, ec).lexically_normal().wstring();
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    const fsx::path parent = fsx::path(s).parent_path();
    return parent.empty() ? root : parent;
}

// True when pid belongs to a process started more than 5 s after 'since' (local "YYYY-MM-DDTHH:MM:SS"): the waiter file's
// process is gone and its pid reused, so the file is stale (GpuLock.ps1 v1.83).
bool reusedPid(uint32_t pid, const std::string& since)
{
    SYSTEMTIME local{}, utc{};
    if (sscanf_s(since.c_str(), "%hu-%hu-%huT%hu:%hu:%hu", &local.wYear, &local.wMonth, &local.wDay, &local.wHour, &local.wMinute, &local.wSecond) != 6) return false;
    FILETIME sinceTime{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &sinceTime)) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = GetProcessTimes(h, &created, &exited, &kernel, &user) != 0;
    CloseHandle(h);
    auto ticks = [](FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    return ok && ticks(created) > ticks(sinceTime) + 5ull * 10000000ull;
}

// Whether a waiter (kind, since, pid) takes its turn before this one: first come ("since", then pid), whatever the kind
// - GpuLock.ps1 v1.85 (v1.83 put timing first: a 31-minute timing batch held four sessions up).
bool turnBefore(const std::string&, const std::string& since, uint32_t pid, const std::string&, const std::string& selfSince, uint32_t self)
{
    const int order = since.compare(selfSince);
    return order < 0 || (order == 0 && pid < self);
}

// A live waiter in .gpulock/waiting whose turn comes before this correctness slice's (INTERFACES 3.3 v1.85: first come -
// "since", then pid - whatever the kind); removes the records of waiters whose process is gone or whose pid was reused.
bool waiterAhead(const std::filesystem::path& dir, uint32_t self, const std::string& selfSince)
{
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return false;
    bool found = false;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    {
        if (e.path().extension() != ".json") continue;
        const std::string text = readShared(e.path());
        const std::string pid = field(text, "pid");
        const uint32_t p = pid.empty() ? 0 : (uint32_t)std::strtoul(pid.c_str(), nullptr, 10);
        if (p == self) continue;
        const std::string since = field(text, "since");
        if (p == 0 || !processAlive(p) || reusedPid(p, since))
        {
            std::filesystem::remove(e.path(), ec);
            continue;
        }
        if (turnBefore(field(text, "kind"), since, p, "correctness", selfSince, self)) found = true;
    }
    return found;
}

std::string sliceText(uint32_t k, uint32_t total) { return "slice " + std::to_string(k) + "/" + (total ? std::to_string(total) : std::string("?")); }
} // namespace

GpuSlice::GpuSlice(std::filesystem::path lockDir, std::string track, std::string what)
    : m_dir(std::move(lockDir)), m_track(std::move(track)), m_what(std::move(what))
{
    // One .gpulock for the main checkout and all its worktrees (v1.83; UNX_GPU_LOCK_DIR overrides, as for GpuLock.ps1).
    char* env = nullptr;
    size_t envSize = 0;
    if (_dupenv_s(&env, &envSize, "UNX_GPU_LOCK_DIR") == 0 && env && *env)
        m_dir = env;
    else if (m_dir.filename() == ".gpulock")
        m_dir = mainCheckout(m_dir.parent_path()) / ".gpulock";
    std::free(env);
    std::filesystem::create_directories(m_dir);
    m_mutex = CreateMutexW(nullptr, FALSE, L"Local\\UnravelNext.GpuMeasurement");
    if (!m_mutex) fail("gpu slice: CreateMutex failed (%lu)", GetLastError());
}

GpuSlice::~GpuSlice()
{
    if (m_held) release();
    if (m_mutex) CloseHandle((HANDLE)m_mutex);
}

double GpuSlice::acquire()
{
    if (m_held) return 0;
    // Run under a GpuLock.ps1 wrapper (it holds the lock for the whole run): waiting for our own wrapper would deadlock
    // (2026-09-26: a wrapped reference render held the lock 45 minutes while its own slices waited). Work under the
    // wrapper's lock instead; no slice records.
    {
        const std::string holder = readShared(m_dir / "current.json");
        const std::string pid = holder.empty() ? std::string() : field(holder, "pid");
        if (!pid.empty() && isAncestor((uint32_t)std::strtoul(pid.c_str(), nullptr, 10)))
        {
            if (!m_inherited)
                logf("gpu slice: running under the GPU lock of a parent process (%s) - no slices, other sessions wait for the whole run. "
                     "The GPU reference takes the lock itself: run it directly, not under Tools/CI/GpuLock.ps1\n",
                     field(holder, "command").c_str());
            m_inherited = true;
            m_held = true;
            m_start = std::chrono::steady_clock::now();
            return 0;
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t self = GetCurrentProcessId();
    const std::filesystem::path hold = m_dir / "HOLD", current = m_dir / "current.json", history = m_dir / "history.log";
    const std::filesystem::path waitingDir = m_dir / "waiting", waitingFile = waitingDir / (std::to_string(self) + ".json");
    std::filesystem::create_directories(waitingDir);
    const std::string since = nowText();
    writeReplace(waitingFile, "{\"track\":\"" + m_track + "\",\"kind\":\"correctness\",\"pid\":" + std::to_string(self) + ",\"since\":\"" + since +
                                  "\",\"command\":\"" + jsonEscape(m_what) + "\"}");
    struct RemoveOnExit
    {
        std::filesystem::path f;
        ~RemoveOnExit()
        {
            std::error_code ec;
            std::filesystem::remove(f, ec);
        }
    } removeWaiting{ waitingFile };
    bool heldAnnounced = false, yieldAnnounced = false, waitAnnounced = false;
    for (;;)
    {
        if (std::filesystem::exists(hold))
        {
            if (!heldAnnounced) logf("gpu slice: .gpulock/HOLD in place - GPU work paused\n");
            heldAnnounced = true;
            Sleep(1000);
            continue;
        }
        if (waiterAhead(waitingDir, self, since))
        {
            if (!yieldAnnounced) logf("gpu slice: in line behind an earlier waiter (or a timing measurement)\n");
            yieldAnnounced = true;
            Sleep(250);
            continue;
        }
        const DWORD r = WaitForSingleObject((HANDLE)m_mutex, 10000);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED)
        {
            // A holder that died without its release (abandoned mutex, or its record left with its process gone).
            const std::string stale = readShared(current);
            if (!stale.empty())
            {
                const std::string pid = field(stale, "pid");
                const bool gone = !pid.empty() && !processAlive((uint32_t)std::strtoul(pid.c_str(), nullptr, 10));
                if (r == WAIT_ABANDONED || gone)
                    appendHistory(history, nowText() + " stale release " + field(stale, "track") + " (" + field(stale, "kind") + ") holder pid " + pid + " gone :: " +
                                               field(stale, "command"));
            }
            else if (r == WAIT_ABANDONED) appendHistory(history, nowText() + " stale release (unknown holder)");
            // HOLD or a waiter ahead may have appeared while this process waited for the mutex.
            if (std::filesystem::exists(hold) || waiterAhead(waitingDir, self, since))
            {
                ReleaseMutex((HANDLE)m_mutex);
                Sleep(250);
                continue;
            }
            break;
        }
        if (r != WAIT_TIMEOUT) fail("gpu slice: waiting for the GPU lock failed (%lu)", GetLastError());
        if (!waitAnnounced)
        {
            const std::string holder = readShared(current);
            logf("gpu slice: waiting for the GPU lock; held by: %s\n", holder.empty() ? "(unknown)" : holder.c_str());
            waitAnnounced = true;
        }
    }
    const std::string started = nowText();
    writeReplace(current, "{\"track\":\"" + m_track + "\",\"kind\":\"correctness\",\"pid\":" + std::to_string(self) + ",\"started\":\"" + started +
                              "\",\"timeoutMinutes\":1,\"command\":\"" + jsonEscape(m_what) + "\"}");
    ++m_slices;
    appendHistory(history, started + " acquire " + m_track + " (correctness) :: " + sliceText(m_slices, m_totalEstimate) + " " + m_what);
    m_held = true;
    m_start = std::chrono::steady_clock::now();
    const double waited = std::chrono::duration<double>(m_start - t0).count();
    m_waited += waited;
    return waited;
}

void GpuSlice::release()
{
    if (!m_held) return;
    if (m_inherited)
    {
        m_held = false;  // the parent's lock stays; nothing of ours to release
        return;
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    m_longest = std::max(m_longest, sec);
    appendHistory(m_dir / "history.log", nowText() + " release " + m_track + " (correctness) exit 0 " + sliceText(m_slices, m_totalEstimate) + " " +
                                             std::to_string((long long)(sec * 1000)) + " ms" + (sec > kSliceSeconds + 5 ? " LONG_SLICE" : ""));
    std::error_code ec;
    std::filesystem::remove(m_dir / "current.json", ec);
    ReleaseMutex((HANDLE)m_mutex);
    m_held = false;
    // A session already blocked on the mutex receives it now; the pause lets a timing waiter's record be seen too.
    Sleep(250);
}

bool GpuSlice::sliceExpired() const
{
    return m_held && !m_inherited && std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count() >= kSliceSeconds;
}
} // namespace unx::reference::gpu
