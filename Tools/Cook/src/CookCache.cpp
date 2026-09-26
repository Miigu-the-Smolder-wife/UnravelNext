#include "CookCache.h"

#include "unx/core/Log.h"
#include "unx/core/Sha256.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <windows.h>

namespace unx::cook
{
namespace
{
std::array<uint8_t, 32> bytesOfHex(const char* hex)
{
    std::array<uint8_t, 32> out{};
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    for (int i = 0; i < 32; ++i) out[i] = (uint8_t)(v(hex[2 * i]) * 16 + v(hex[2 * i + 1]));
    return out;
}

constexpr size_t kHeaderBytes = 8 + 32 + 32;

std::filesystem::path entryPath(const std::string& directory, const char* kind, const Key& key)
{
    return std::filesystem::path(directory) / kind / (hexOf(key) + ".unxcook");
}
} // namespace

std::string hexOf(const Key& key)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t c : key)
    {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::string cacheDirectory(const std::string& explicitDirectory, bool set)
{
    if (set) return explicitDirectory;
    // GetEnvironmentVariableW sees values the process set after start-up (the Unity editor sets it), unlike the CRT's copy.
    wchar_t buffer[1024];
    const DWORD n = GetEnvironmentVariableW(L"UNX_COOK_CACHE", buffer, 1024);
    if (n == 0 || n >= 1024) return {};
    return std::filesystem::path(std::wstring(buffer, n)).string();
}

bool readEntry(const std::string& directory, const char* kind, const Key& key, uint32_t magic, uint32_t format, const char* sourceHashHex,
               std::vector<uint8_t>& payload)
{
    const std::filesystem::path path = entryPath(directory, kind, key);
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < kHeaderBytes + 32) return false;
    uint32_t head[2];
    std::memcpy(head, file.data(), 8);
    const std::array<uint8_t, 32> source = bytesOfHex(sourceHashHex);
    if (head[0] != magic || head[1] != format || std::memcmp(file.data() + 8, source.data(), 32) != 0 || std::memcmp(file.data() + 40, key.data(), 32) != 0)
        return false;
    const size_t bytes = file.size() - kHeaderBytes - 32;
    Sha256 h;
    h.update(file.data() + kHeaderBytes, bytes);
    const std::array<uint8_t, 32> d = h.finish();
    if (std::memcmp(d.data(), file.data() + kHeaderBytes + bytes, 32) != 0)
    {
        logf("UnravelNext cook cache: %s is corrupted (checksum); cooking it again\n", path.string().c_str());
        return false;
    }
    payload.assign(file.begin() + kHeaderBytes, file.begin() + kHeaderBytes + bytes);
    return true;
}

void writeEntry(const std::string& directory, const char* kind, const Key& key, uint32_t magic, uint32_t format, const char* sourceHashHex,
                const std::vector<uint8_t>& payload)
{
    std::error_code ec;
    const std::filesystem::path path = entryPath(directory, kind, key);
    std::filesystem::create_directories(path.parent_path(), ec);
    Sha256 h;
    h.update(payload.data(), payload.size());
    const std::array<uint8_t, 32> d = h.finish();
    const uint32_t head[2] = { magic, format };
    const std::array<uint8_t, 32> source = bytesOfHex(sourceHashHex);
    const std::filesystem::path tmp = path.string() + ".tmp" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetCurrentThreadId());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            logf("UnravelNext cook cache: cannot write %s\n", tmp.string().c_str());
            return;
        }
        f.write((const char*)head, 8);
        f.write((const char*)source.data(), 32);
        f.write((const char*)key.data(), 32);
        f.write((const char*)payload.data(), (std::streamsize)payload.size());
        f.write((const char*)d.data(), 32);
        if (!f)
        {
            logf("UnravelNext cook cache: writing %s failed\n", tmp.string().c_str());
            f.close();
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}
} // namespace unx::cook
