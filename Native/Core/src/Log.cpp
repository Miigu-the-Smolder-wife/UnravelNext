#include "unx/core/Log.h"

#include <cstdio>
#include <mutex>

namespace unx
{
namespace
{
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
} // namespace

std::string formatv(const char* format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    std::string out(n > 0 ? (size_t)n : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), (size_t)n + 1, format, args);
    return out;
}

std::string format(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string s = formatv(fmt, args);
    va_end(args);
    return s;
}

void logf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string s = formatv(fmt, args);
    va_end(args);
    std::lock_guard lock(g_logMutex);
    std::fputs(s.c_str(), stderr);
    if (g_logFile)
    {
        std::fputs(s.c_str(), g_logFile);
        std::fflush(g_logFile);
    }
}

void logOpenFile(const std::string& path)
{
    std::lock_guard lock(g_logMutex);
    if (g_logFile) std::fclose(g_logFile);
    g_logFile = nullptr;
    fopen_s(&g_logFile, path.c_str(), "wb");
}

void fail(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string s = formatv(fmt, args);
    va_end(args);
    throw Error(s);
}
} // namespace unx
