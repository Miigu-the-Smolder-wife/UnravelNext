// unx-kernel: cs_6_6 main
// Caustics cost study (Gates/CausticBench.cpp): WaterCaustics.hlsl with its study hooks defined. P[1].w selects the
// work: bits 0-4 the slices run; bit 8 rays only (the nine rays and the block's deviation, no slices); bit 9 the raster
// without atomics (the deposits summed in a register); bit 11 the raster's edge integrals skipped (a constant
// overlap), bit 12 no raster at all (cells placed, their level chosen); bit 10 counters (P[1].y a raw buffer of WATER_STUDY_WORDS words,
// wave sums then one atomic per wave): see the word layout below. Counting is its own run; time the others.
#define WATER_CAUSTIC_STUDY 1
// Counter words: 0 blocks past the early out, 1 blocks with all nine rays, 2+s merged quads, 7+s cells, 12+s raster
// texel iterations, 17+s slice atomics, 22+s level atomics, 27+L cells by level (L < 11), 38 raster iterations summed
// per thread, 39 per wave its lanes' max x its active lanes (SIMD work: 39 / 38 is the divergence loss)
#define WATER_STUDY_WORDS 40u
static uint g_mode;
static float g_sink;
static uint g_count[WATER_STUDY_WORDS];
#define STUDY_COUNT(w) \
    if (g_mode & 1024u) g_count[w] += 1
#define CAUSTIC_TEX_ADD(tex, at, slice, v)                  \
    {                                                        \
        STUDY_COUNT(17 + (slice));                           \
        if (g_mode & 512u) g_sink += float(v);               \
        else InterlockedAdd(tex[uint3(at, slice)], v);       \
    }
#define CAUSTIC_LEVEL_ADD(buf, address, v)                   \
    {                                                        \
        STUDY_COUNT(22 + slice);                             \
        if (g_mode & 512u) g_sink += float(v);               \
        else buf.InterlockedAdd(address, v);                 \
    }
#define CAUSTIC_STUDY_CELL(L, slice) \
    STUDY_COUNT(7 + (slice));        \
    STUDY_COUNT(27 + min(L, 10u))
#define CAUSTIC_STUDY_TEXEL(slice) \
    STUDY_COUNT(12 + (slice));     \
    STUDY_COUNT(38)
#define CAUSTIC_STUDY_MERGED(slice) STUDY_COUNT(2 + (slice))
#define CAUSTIC_STUDY_BEGIN()                                     \
    g_mode = P[1].w;                                              \
    g_sink = 0;                                                   \
    [unroll] for (uint w0 = 0; w0 < WATER_STUDY_WORDS; ++w0) g_count[w0] = 0; \
    STUDY_COUNT(0)
#define CAUSTIC_STUDY_RAYS(sum)                                                     \
    if (all) STUDY_COUNT(1);                                                        \
    if (g_mode & 256u)                                                              \
    {                                                                               \
        const float3 v_ = sum;                                                      \
        if (v_.x + v_.y + v_.z == 12345.678f) caustics[uint3(0, 0, 0)] = 1;         \
        CAUSTIC_STUDY_END();                                                        \
        return;                                                                     \
    }
#define CAUSTIC_STUDY_SKIP(slice) ((g_mode & (1u << (slice))) == 0)
#define CAUSTIC_STUDY_NO_EDGES() ((g_mode & 2048u) != 0)
bool studyNoRaster(float keep)
{
    if ((g_mode & 4096u) == 0) return false;
    g_sink += keep;
    return true;
}
#define CAUSTIC_STUDY_NO_RASTER() studyNoRaster(mn.x + mx.y + float(L))
#define CAUSTIC_STUDY_END()                                                                          \
    {                                                                                                \
        if (g_sink == 12345.678f) caustics[uint3(0, 0, 0)] = 1;                                      \
        if (g_mode & 1024u)                                                                          \
        {                                                                                            \
            RWByteAddressBuffer counters_ = ResourceDescriptorHeap[P[1].y];                         \
            g_count[39] = WaveActiveMax(g_count[38]) * WaveActiveCountBits(true);                    \
            [unroll] for (uint w1 = 0; w1 < WATER_STUDY_WORDS; ++w1)                                 \
            {                                                                                        \
                const uint s_ = w1 == 39 ? g_count[39] : WaveActiveSum(g_count[w1]);                 \
                if (WaveIsFirstLane() && s_) counters_.InterlockedAdd(4 * w1, s_);                   \
            }                                                                                        \
        }                                                                                            \
    }
#include "../WaterCaustics.hlsl"
