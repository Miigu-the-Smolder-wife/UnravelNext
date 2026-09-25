#pragma once
// Hold files that pause CPU-heavy reference work (render queue, census, material studies).
//   GPU lock record (.gpulock/current.json, Tools/CI/GpuLock.ps1): {"track", "kind", "pid", ...}. It holds only while
//   the holder process is alive (a holder killed without cleanup cannot stop work forever) and only for "kind":
//   "timing" (a performance measurement); "correctness" runs do not need a quiet CPU. A record without "kind" is the
//   older format and counts as timing.
//   Manual marker (.gpulock/HOLD, the user is gaming): holds while it exists, whatever it contains.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <windows.h>

namespace unx::reference
{
inline std::string holdRecordField(const std::string& text, const char* key)  // string value of "key": "value"
{
    const size_t k = text.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return {};
    const size_t c = text.find(':', k);
    const size_t q0 = c == std::string::npos ? std::string::npos : text.find('"', c);
    const size_t q1 = q0 == std::string::npos ? std::string::npos : text.find('"', q0 + 1);
    return q1 == std::string::npos ? std::string() : text.substr(q0 + 1, q1 - q0 - 1);
}

inline bool holdActive(const std::filesystem::path& f)
{
    std::error_code ec;
    if (!std::filesystem::exists(f, ec)) return false;
    std::ifstream in(f);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t k = text.find("\"pid\"");
    const size_t c = k == std::string::npos ? std::string::npos : text.find(':', k);
    const unsigned long pid = c == std::string::npos ? 0 : std::strtoul(text.c_str() + c + 1, nullptr, 10);
    if (pid == 0) return true;  // manual marker (no holder process)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;  // holder gone: stale record
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    // A record older than the process now using that pid (e.g. left behind by a reboot) is stale: the holder was
    // created before it wrote "started" (local time, yyyy-mm-ddThh:mm:ss), a reused pid only after.
    bool reused = false;
    FILETIME created{}, exited{}, kernel{}, user{};
    const std::string started = holdRecordField(text, "started");
    SYSTEMTIME st{};
    if (alive && started.size() >= 19 && GetProcessTimes(h, &created, &exited, &kernel, &user) &&
        sscanf_s(started.c_str(), "%4hu-%2hu-%2huT%2hu:%2hu:%2hu", &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond) == 6)
    {
        FILETIME localCreated{}, recordTime{};
        if (FileTimeToLocalFileTime(&created, &localCreated) && SystemTimeToFileTime(&st, &recordTime))
        {
            ULARGE_INTEGER cr, rt;
            cr.LowPart = localCreated.dwLowDateTime;
            cr.HighPart = localCreated.dwHighDateTime;
            rt.LowPart = recordTime.dwLowDateTime;
            rt.HighPart = recordTime.dwHighDateTime;
            reused = cr.QuadPart > rt.QuadPart + 2ull * 10000000ull;  // created more than 2 s after the record
        }
    }
    CloseHandle(h);
    if (!alive || reused) return false;
    const std::string kind = holdRecordField(text, "kind");
    return kind.empty() || kind == "timing";
}
} // namespace unx::reference
