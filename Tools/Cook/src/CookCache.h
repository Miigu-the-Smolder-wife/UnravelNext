#pragma once
// Disk entries of the cook caches (C1): <directory>/<kind>/<key hex>.<ext>, each file = magic, format version, source hash
// (32 B), key (32 B), payload, SHA-256 of the payload (32 B). A torn, truncated or corrupted file, or one written by other
// code (source hash), reads as missing; writes go to a temporary file renamed into place.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace unx::cook
{
using Key = std::array<uint8_t, 32>;

std::string hexOf(const Key& key);
// The directory: the explicit one when 'set', else the environment variable UNX_COOK_CACHE (empty: no disk cache).
std::string cacheDirectory(const std::string& explicitDirectory, bool set);
bool readEntry(const std::string& directory, const char* kind, const Key& key, uint32_t magic, uint32_t format, const char* sourceHashHex,
               std::vector<uint8_t>& payload);
void writeEntry(const std::string& directory, const char* kind, const Key& key, uint32_t magic, uint32_t format, const char* sourceHashHex,
                const std::vector<uint8_t>& payload);
} // namespace unx::cook
