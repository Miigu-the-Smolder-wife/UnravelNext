#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace unx::render::detail
{
// SceneUpdate.hlsl's existing layout: 4-byte headers, aligned to 16 bytes,
// followed by one 16-byte payload per header. Only writes to the upload mapping;
// no reads or read/modify/write operations on write-combined memory.
class SceneUpdateWriter
{
public:
    static uint64_t headerBytes(uint64_t count) { return (count * 4 + 15) & ~uint64_t(15); }
    static uint64_t bytes(uint64_t count) { return headerBytes(count) + count * 16; }

    SceneUpdateWriter(uint8_t* destination, uint32_t count)
        : m_headers(destination), m_payload(destination + headerBytes(count)) {}

    void append(uint32_t target, uint32_t first, const void* rows, uint32_t count)
    {
        if (count == 0) return;
        for (uint32_t k = 0; k < count; ++k)
        {
            const uint32_t header = target << 28 | (first + k);
            std::memcpy(m_headers + (size_t(m_count) + k) * 4, &header, 4);
        }
        std::memcpy(m_payload + size_t(m_count) * 16, rows, size_t(count) * 16);
        m_count += count;
    }

    uint32_t count() const { return m_count; }

    // Runtime mirrors may end partway through the final 16-byte row. Preserve
    // the old zero-filled tail without reading past the mirror allocation.
    void appendBytes(uint32_t target, uint32_t first, const uint8_t* source, size_t bytes)
    {
        const uint32_t fullRows = static_cast<uint32_t>(bytes / 16);
        append(target, first, source, fullRows);
        if (bytes % 16 != 0)
        {
            uint8_t tail[16]{};
            std::memcpy(tail, source + size_t(fullRows) * 16, bytes % 16);
            append(target, first + fullRows, tail, 1);
        }
    }

private:
    uint8_t* m_headers;
    uint8_t* m_payload;
    uint32_t m_count = 0;
};
} // namespace unx::render::detail
