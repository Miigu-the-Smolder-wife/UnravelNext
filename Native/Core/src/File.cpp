#include "unx/core/File.h"

#include "unx/core/Log.h"

#include <windows.h>

#include <fstream>
#include <sstream>

namespace unx
{
std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("cannot read %s", path.string().c_str());
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) fail("cannot read %s", path.string().c_str());
    std::vector<uint8_t> bytes((size_t)f.tellg());
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize)bytes.size());
    return bytes;
}

void writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) fail("cannot write %s", path.string().c_str());
    f.write(text.data(), (std::streamsize)text.size());
}

std::filesystem::path executableDirectory()
{
    wchar_t buffer[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, n)).parent_path();
}
} // namespace unx
