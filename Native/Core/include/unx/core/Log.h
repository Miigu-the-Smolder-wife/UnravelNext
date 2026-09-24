#pragma once
#include <cstdarg>
#include <stdexcept>
#include <string>

namespace unx
{
// Formatted log line to stderr (and the log file, if one is open). Thread-safe.
void logf(const char* format, ...);
void logOpenFile(const std::string& path);

std::string formatv(const char* format, va_list args);
std::string format(const char* format, ...);

// Every failure in the engine is an exception carrying a readable message; nothing fails silently.
struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
[[noreturn]] void fail(const char* format, ...);
} // namespace unx
