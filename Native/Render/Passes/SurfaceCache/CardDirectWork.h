#ifndef UNX_CARD_DIRECT_WORK_H
#define UNX_CARD_DIRECT_WORK_H
// One compact record names sixteen original direct-shadow threads. Nonuniform
// slots use up to four contiguous blocks; uniform slots use the existing even
// x/even y texels in a single block. No light, texel, or ray identity is changed.
#ifdef __cplusplus
#include <cstdint>
namespace unx::render::refl::detail {
using uint = std::uint32_t;
#define CL_WORK_CONSTEXPR constexpr
#else
#define CL_WORK_CONSTEXPR
#endif

CL_WORK_CONSTEXPR uint clDirectWorkPack(uint tile, uint slot, uint quarter, bool coarse)
{
    return tile | (slot << 16) | (quarter << 20) | (coarse ? 1u << 22 : 0u);
}
CL_WORK_CONSTEXPR bool clDirectWorkPresent(uint validLo, uint validHi, uint quarter, bool coarse)
{
    if (coarse) return ((validLo | validHi) & 0x00550055u) != 0;
    return (((quarter < 2 ? validLo : validHi) >> ((quarter & 1u) * 16)) & 0xFFFFu) != 0;
}
CL_WORK_CONSTEXPR uint clDirectWorkThread(uint record, uint lane)
{
    const uint tile = record & 0xFFFFu, slot = (record >> 16) & 15u;
    const uint texel = (record & (1u << 22)) != 0 ? ((lane & 3u) * 2 + (lane >> 2) * 16) : ((record >> 20) & 3u) * 16 + lane;
    return tile * 576 + slot * 64 + texel;
}

#undef CL_WORK_CONSTEXPR
#ifdef __cplusplus
} // namespace unx::render::refl::detail
#endif
#endif
