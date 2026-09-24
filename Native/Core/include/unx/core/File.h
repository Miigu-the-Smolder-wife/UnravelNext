#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace unx
{
std::string readTextFile(const std::filesystem::path& path);
std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path);
void writeTextFile(const std::filesystem::path& path, const std::string& text);
// Directory holding the running executable.
std::filesystem::path executableDirectory();
} // namespace unx
