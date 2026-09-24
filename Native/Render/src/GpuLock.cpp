#include "unx/render/GpuLock.h"

#include "unx/core/Log.h"
#include "unx/render/Tracks.h"

#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

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
