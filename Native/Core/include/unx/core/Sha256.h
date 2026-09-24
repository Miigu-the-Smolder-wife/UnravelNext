#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace unx
{
// SHA-256 (FIPS 180-4). Used for quality-parameter and content identities that must be checkable with standard tools.
class Sha256
{
public:
    Sha256();
    void update(const void* data, size_t size);
    void update(std::string_view text) { update(text.data(), text.size()); }
    std::array<uint8_t, 32> finish();
    static std::string hex(std::string_view text);

private:
    void block(const uint8_t* p);
    uint32_t m_state[8];
    uint8_t m_buffer[64];
    uint64_t m_length = 0;
    size_t m_fill = 0;
};
} // namespace unx
