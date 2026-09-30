#include "unx/render/GpuLock.h"

#include "unx/core/Log.h"
#include "unx/render/TrackPending.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

namespace unx::render
{
std::string requireGpuLock(const char* what)
{
    char* value = nullptr;
    size_t size = 0;
    std::string track;
    if (_dupenv_s(&value, &size, "UNX_GPU_LOCK") == 0 && value)
    {
        track = value;
        std::free(value);
    }
    if (track.empty())
        fail("%s is a performance measurement: run it through Tools/CI/GpuLock.ps1 -Track <track> -- <command> "
             "(one measurement at a time across sessions; correctness runs need no lock)",
             what);
    return track;
}

namespace
{
namespace fs = std::filesystem;

std::string localTime()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[32];
    snprintf(s, sizeof s, "%04u-%02u-%02uT%02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
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

// The value of "key" in a one-line JSON object (strings and numbers only); empty when absent.
std::string jsonField(const std::string& json, const char* key)
{
    const std::string k = std::string("\"") + key + "\":";
    size_t i = json.find(k);
    if (i == std::string::npos) return {};
    i += k.size();
    while (i < json.size() && json[i] == ' ') ++i;
    if (i < json.size() && json[i] == '"')
    {
        std::string v;
        for (++i; i < json.size() && json[i] != '"'; ++i)
        {
            if (json[i] == '\\' && i + 1 < json.size()) ++i;
            v += json[i];
        }
        return v;
    }
    size_t e = i;
    while (e < json.size() && json[e] != ',' && json[e] != '}') ++e;
    return json.substr(i, e - i);
}

std::string readAll(const fs::path& p)
{
    for (int k = 0; k < 20; ++k)
    {
        std::ifstream in(p, std::ios::binary);
        if (!in)
        {
            std::error_code ec;
            if (!fs::exists(p, ec)) return {};
            Sleep(25);
            continue;
        }
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }
    return {};
}

// Writes a temporary file and renames it over 'p' (readers open with delete sharing), as GpuLock.ps1 does.
void writeAtomic(const fs::path& p, const std::string& text)
{
    const fs::path tmp = p.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
    }
    for (int k = 0; k < 40; ++k)
    {
        if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        Sleep(25);
    }
    std::error_code ec;
    fs::remove(tmp, ec);
    logf("GpuLockSlice: could not write %s\n", p.string().c_str());
}

bool processAlive(uint32_t pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
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
    const std::string text = trimmed(readAll(dotGit));
    if (text.rfind("gitdir:", 0) != 0) return root;
    fsx::path gitDir = trimmed(text.substr(7));
    if (gitDir.is_relative()) gitDir = root / gitDir;
    fsx::path common = gitDir;
    const std::string commonDir = trimmed(readAll(gitDir / "commondir"));
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

// Whether a waiter (kind, since, pid) takes its turn before this one: timing before correctness, then first come
// ("since", then pid) - GpuLock.ps1 v1.83.
bool turnBefore(const std::string& kind, const std::string& since, uint32_t pid, const std::string& selfKind, const std::string& selfSince, uint32_t self)
{
    const int rank = kind == "timing" ? 0 : 1, selfRank = selfKind == "timing" ? 0 : 1;
    if (rank != selfRank) return rank < selfRank;
    const int order = since.compare(selfSince);
    return order < 0 || (order == 0 && pid < self);
}

std::string defaultLockDir()
{
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, "UNX_GPU_LOCK_DIR") == 0 && value)
    {
        std::string dir = value;
        std::free(value);
        if (!dir.empty()) return dir;
    }
    std::error_code ec;
    for (fs::path p = fs::current_path(ec); !p.empty(); p = p.parent_path())
    {
        if (fs::exists(p / ".git", ec)) return (mainCheckout(p) / ".gpulock").string();  // one folder for all worktrees (v1.83)
        if (fs::is_directory(p / ".gpulock", ec)) return (p / ".gpulock").string();
        if (p == p.root_path()) break;
    }
    fail("GpuLockSlice: no .gpulock folder above %s (run from the repository or set UNX_GPU_LOCK_DIR)", fs::current_path(ec).string().c_str());
}
} // namespace

GpuLockSlice::GpuLockSlice(std::string track, std::string kind, std::string what, std::string lockDir, std::string mutexName)
    : m_track(std::move(track)), m_kind(std::move(kind)), m_what(std::move(what)), m_dir(lockDir.empty() ? defaultLockDir() : std::move(lockDir))
{
    if (m_kind != "timing" && m_kind != "correctness") fail("GpuLockSlice: kind '%s' (timing or correctness)", m_kind.c_str());
    std::error_code ec;
    fs::create_directories(fs::path(m_dir) / "waiting", ec);
    const std::wstring name(mutexName.begin(), mutexName.end());
    m_mutex = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!m_mutex) fail("GpuLockSlice: CreateMutex failed (%lu)", GetLastError());
}

GpuLockSlice::~GpuLockSlice()
{
    if (m_held) release(0);
    if (m_mutex) CloseHandle(m_mutex);
}

void GpuLockSlice::appendHistory(const std::string& line)
{
    const fs::path p = fs::path(m_dir) / "history.log";
    const std::string text = line + "\r\n";
    for (int k = 0; k < 40; ++k)
    {
        HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
            CloseHandle(h);
            return;
        }
        Sleep(25);
    }
    logf("GpuLockSlice: could not append to %s\n", p.string().c_str());
}

// Why this process must not take the lock now (empty: it may): HOLD, or a live waiter whose turn comes first (v1.83:
// timing before correctness, then "since", then pid). Waiting files of dead processes and of reused pids are removed.
std::string GpuLockSlice::blocker(const std::string& since)
{
    std::error_code ec;
    const fs::path hold = fs::path(m_dir) / "HOLD";
    if (fs::exists(hold, ec))
    {
        std::string reason = readAll(hold);
        while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r' || reason.back() == ' ')) reason.pop_back();
        return "HOLD: " + (reason.empty() ? std::string("(no reason given)") : reason);
    }
    const uint32_t self = GetCurrentProcessId();
    for (const fs::directory_entry& e : fs::directory_iterator(fs::path(m_dir) / "waiting", ec))
    {
        if (e.path().extension() != ".json") continue;
        const std::string json = readAll(e.path());
        const uint32_t pid = (uint32_t)std::strtoul(jsonField(json, "pid").c_str(), nullptr, 10);
        if (pid == 0 || pid == self) continue;
        const std::string kind = jsonField(json, "kind"), waiting = jsonField(json, "since");
        if (!processAlive(pid) || reusedPid(pid, waiting))
        {
            std::error_code rc;
            fs::remove(e.path(), rc);
            continue;
        }
        if (turnBefore(kind, waiting, pid, m_kind, since, self))
            return "in line behind " + jsonField(json, "track") + " (" + kind + ", pid " + std::to_string(pid) + ", since " + waiting + ")";
    }
    return {};
}

bool GpuLockSlice::acquire(std::chrono::milliseconds waitLimit, const std::string& label)
{
    if (m_held) fail("GpuLockSlice::acquire: already held (%s)", m_label.c_str());
    const auto start = std::chrono::steady_clock::now();
    const uint32_t self = GetCurrentProcessId();
    const fs::path waitFile = fs::path(m_dir) / "waiting" / (std::to_string(self) + ".json");
    const std::string command = m_what + (label.empty() ? "" : " " + label);
    const std::string since = localTime();
    writeAtomic(waitFile, "{\"track\":\"" + jsonEscape(m_track) + "\",\"kind\":\"" + m_kind + "\",\"pid\":" + std::to_string(self) + ",\"since\":\"" + since +
                              "\",\"command\":\"" + jsonEscape(command) + "\"}");
    m_lastBlocker.clear();
    bool got = false, abandoned = false;
    while (std::chrono::steady_clock::now() - start < waitLimit)
    {
        const std::string b = blocker(since);
        if (!b.empty())
        {
            m_lastBlocker = b;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        const long long left = std::chrono::duration_cast<std::chrono::milliseconds>(waitLimit - (std::chrono::steady_clock::now() - start)).count();
        const DWORD r = WaitForSingleObject(m_mutex, (DWORD)std::clamp<long long>(left, 0, 1000));
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) continue;
        abandoned = r == WAIT_ABANDONED;
        const std::string late = blocker(since);  // a waiter ahead or HOLD appeared while this one waited on the mutex
        if (!late.empty())
        {
            m_lastBlocker = late;
            ReleaseMutex(m_mutex);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        got = true;
        break;
    }
    std::error_code ec;
    fs::remove(waitFile, ec);
    if (!got) return false;
    // A holder that died without its release: its current.json is left behind (a releasing holder deletes it first).
    const fs::path current = fs::path(m_dir) / "current.json";
    const std::string stale = readAll(current);
    if (!stale.empty())
    {
        const uint32_t pid = (uint32_t)std::strtoul(jsonField(stale, "pid").c_str(), nullptr, 10);
        if (abandoned || !processAlive(pid))
            appendHistory(localTime() + " stale release " + jsonField(stale, "track") + " (" + jsonField(stale, "kind") + ") holder pid " + std::to_string(pid) +
                          " gone :: " + jsonField(stale, "command"));
    }
    else if (abandoned) appendHistory(localTime() + " stale release (unknown holder)");
    m_label = label;
    m_since = std::chrono::steady_clock::now();
    const std::string started = localTime();
    writeAtomic(current, "{\"track\":\"" + jsonEscape(m_track) + "\",\"kind\":\"" + m_kind + "\",\"pid\":" + std::to_string(self) + ",\"started\":\"" + started +
                             "\",\"timeoutMinutes\":0.25,\"command\":\"" + jsonEscape(command) + "\"}");
    appendHistory(started + " acquire " + m_track + " (" + m_kind + ") :: " + command);
    SetEnvironmentVariableA("UNX_GPU_LOCK", m_track.c_str());
    _putenv_s("UNX_GPU_LOCK", m_track.c_str());
    m_held = true;
    return true;
}

void GpuLockSlice::release(int exitCode)
{
    if (!m_held) return;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_since).count();
    char tail[96];
    snprintf(tail, sizeof tail, " %.0f ms%s", ms, ms > 15000.0 ? " LONG_SLICE" : "");
    appendHistory(localTime() + " release " + m_track + " (" + m_kind + ") exit " + std::to_string(exitCode) + (m_label.empty() ? "" : " " + m_label) + tail);
    std::error_code ec;
    fs::remove(fs::path(m_dir) / "current.json", ec);
    SetEnvironmentVariableA("UNX_GPU_LOCK", nullptr);
    _putenv_s("UNX_GPU_LOCK", "");
    m_held = false;
    ReleaseMutex(m_mutex);
}

namespace tracks
{
void pending(const char* entry)
{
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard lock(mutex);
    if (seen.insert(entry).second) logf("note: track entry point %s is not implemented yet; it declares no passes (INTERFACES_KO.md 5.2)\n", entry);
}
} // namespace tracks
} // namespace unx::render
