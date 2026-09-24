#pragma once
// 16-sub-sample identity census (ARCHITECTURE 1.2 table rows "가장자리", 7.2 P1 gate). Owner: C.
//
// Reference side: 16 stratified (4 x 4) primary sub-samples per pixel, each giving the identity of the surface hit:
// instance << 32 | mesh triangle, kSkyId for no hit (unx_reference computes them from the scene).
// Engine side: an identity capture (.unxids) the renderer writes from its vis buffer and coverage layer: per pixel a
// list whose first entry is the vis-buffer (pixel-centre) identity and whose remaining entries are the pixel's
// coverage-layer fragments. An entry may name a whole instance (triangle field kAnyTriangle) when the engine drew a
// representation that has no mesh triangle (LOD > 0 clusters, band-C bricks).
//
// "Missed" sub-sample: its identity is in none of: the vis-buffer identities of the pixel's 3x3 neighbourhood, and the
// pixel's own coverage fragments. The gate metric is the fraction of pixels with >= 1 missed sub-sample (P1: <= 0.1%);
// >= 4 and >= 8 (25% / 50% of the pixel area) are reported too, and the distinct-identity counts (>= 2/3/5/9).
// selfCensus() uses the reference's own pixel-centre sample as a 1-sample visibility buffer: that is exactly the
// microbench measurement (Tools/Microbench --only-edges) and the baseline the coverage layer must remove.
//
// File format .unxids (little endian): "UNXIDS1\0", u32 width, u32 height, u32 offsets[width*height + 1],
// u64 ids[offsets[width*height]]; pixel p owns ids[offsets[p] .. offsets[p+1]), rows top to bottom.
#include <cstdint>
#include <filesystem>
#include <vector>

namespace unx::metrics
{
constexpr uint64_t kSkyId = ~0ull;
constexpr uint32_t kAnyTriangle = 0xFFFFFFFFu;

struct IdentityImage
{
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> offsets;  // width * height + 1
    std::vector<uint64_t> ids;
};
IdentityImage readIdentities(const std::filesystem::path& path);
void writeIdentities(const std::filesystem::path& path, const IdentityImage& image);

struct CensusResult
{
    uint64_t pixels = 0;
    double distinct2 = 0, distinct3 = 0, distinct5 = 0, distinct9 = 0;  // fraction of pixels with >= k identities
    double missed1 = 0, missed4 = 0, missed8 = 0;                        // fraction with >= k missed sub-samples
};

// subsamples: width * height * stride identities (stride >= 16; the first 16 are the 4 x 4 grid).
CensusResult census(const std::vector<uint64_t>& subsamples, uint32_t stride, const IdentityImage& engine);
// 1-sample baseline: sub-sample 16 (pixel centre) of each pixel as the vis buffer, no coverage layer (stride 17).
CensusResult selfCensus(const std::vector<uint64_t>& subsamples17, uint32_t width, uint32_t height);
} // namespace unx::metrics
