// VSM transmittance layer (ARCHITECTURE 2.3 [revision 1], COVERAGE_REDESIGN 4.2; INTERFACES 5.6 v1.26): thin casters'
// (leaves, grass blades, strands, wires: narrower than 1.5 texels of the level) shadows as a transmittance function
// T(h) per texel next to the opaque casters' height field. Owner: S.
//
// FrameResources::vsmLayers (raw):
//   words 0 .. pool pages - 1: per physical VSM page its layer page + 1 (0 = no thin casters: T = 1);
//   from VSM_LAYER_BASE: layer pages of VSM_LAYER_PAGE_BYTES each:
//     header 16 B { float hLo, hHi; uint pad[2] }: the light-space height range the knots' heights map to (unorm16);
//     texels: 128 x 128 x { uint2 knots } (16 B: K = 4 knots { h unorm16 | T unorm16 << 16 }, h descending);
//     profile mips 1 (64 x 64) and 2 (32 x 32): the same records for 2 x 2 and 4 x 4 texel blocks (T averaged over the
//     block, knots recompressed), for lookups whose reach spans the block.
// T(h) of a record: 1 for h >= h_1 (above the thin casters), linear between knots, T_K below h_K.
// Filled for dirty pages from V's coverage-mode raster (INTERFACES 5.3 v1.26; VsmLayerPixel / VsmLayerCompress); until
// then every page has no layer and every lookup returns 1.
#ifndef UNX_VSM_LAYER_HLSLI
#define UNX_VSM_LAYER_HLSLI
#include "Passes/Shadow/VsmCommon.hlsli"

#define VSM_LAYER_KNOTS 4u
#define VSM_LAYER_RECORD_BYTES 16u
#define VSM_LAYER_HEADER_BYTES 16u
#define VSM_LAYER_MIP1_OFFSET (VSM_LAYER_HEADER_BYTES + VSM_PAGE * VSM_PAGE * VSM_LAYER_RECORD_BYTES)
#define VSM_LAYER_MIP2_OFFSET (VSM_LAYER_MIP1_OFFSET + (VSM_PAGE / 2) * (VSM_PAGE / 2) * VSM_LAYER_RECORD_BYTES)
#define VSM_LAYER_PAGE_BYTES (VSM_LAYER_MIP2_OFFSET + (VSM_PAGE / 4) * (VSM_PAGE / 4) * VSM_LAYER_RECORD_BYTES)

// First layer page byte offset: after the per-physical-page words, 256-byte aligned.
uint vsmLayerBase(uint poolPages) { return (poolPages * 4 + 255) & ~255u; }

// T(h) of one record (4 knots).
float vsmLayerRecordT(uint4 record, float hLo, float hHi, float h)
{
    const float scale = (hHi - hLo) / 65535.0;
    float prevH = 3.0e38, prevT = 1;
    [unroll] for (uint k = 0; k < VSM_LAYER_KNOTS; ++k)
    {
        const float hk = hLo + (record[k] & 0xFFFFu) * scale, tk = (record[k] >> 16) / 65535.0;
        if (h >= hk) return k == 0 ? 1.0 : lerp(tk, prevT, saturate((h - hk) / max(prevH - hk, 1e-30)));
        prevH = hk;
        prevT = tk;
    }
    return prevT;
}

// Transmittance of the thin casters of level k at light-space uv (m) and height h, from the mip whose texel covers
// 'reach' (the lookup's filter radius): bilinear over the 2 x 2 records around uv (clamped to the page), each record's
// T(h) evaluated at h. Returns 1 where the page is not resident or has no layer.
float vsmLayerTransmittance(ByteAddressBuffer layers, uint poolPages, uint entry, int2 absPage, float2 uv, uint k, float reach, float h)
{
    if (entry == 0) return 1;
    const uint phys = entry & VSM_PHYS_MASK;
    const uint index = layers.Load(phys * 4);
    if (index == 0) return 1;
    const uint base = vsmLayerBase(poolPages) + (index - 1) * VSM_LAYER_PAGE_BYTES;
    const float2 range = asfloat(layers.Load2(base));
    const float texel = vsmTexel(k);
    const uint mip = (uint)clamp(floor(log2(max(reach / texel, 1.0))), 0.0, 2.0);
    const uint res = VSM_PAGE >> mip;
    const uint offset = mip == 0 ? VSM_LAYER_HEADER_BYTES : mip == 1 ? VSM_LAYER_MIP1_OFFSET : VSM_LAYER_MIP2_OFFSET;
    // Position in the page's records of this mip (record centres at integer + 0.5).
    const float2 inPage = (uv / texel - float2(absPage * (int)VSM_PAGE)) / (1u << mip) - 0.5;
    const float2 p0 = clamp(floor(inPage), 0.0, float(res - 1));
    const float2 f = saturate(inPage - p0);
    const uint2 q0 = uint2(p0), q1 = min(q0 + 1, res - 1);
    const uint4 r00 = layers.Load4(base + offset + (q0.y * res + q0.x) * VSM_LAYER_RECORD_BYTES);
    const uint4 r10 = layers.Load4(base + offset + (q0.y * res + q1.x) * VSM_LAYER_RECORD_BYTES);
    const uint4 r01 = layers.Load4(base + offset + (q1.y * res + q0.x) * VSM_LAYER_RECORD_BYTES);
    const uint4 r11 = layers.Load4(base + offset + (q1.y * res + q1.x) * VSM_LAYER_RECORD_BYTES);
    const float t00 = vsmLayerRecordT(r00, range.x, range.y, h), t10 = vsmLayerRecordT(r10, range.x, range.y, h);
    const float t01 = vsmLayerRecordT(r01, range.x, range.y, h), t11 = vsmLayerRecordT(r11, range.x, range.y, h);
    return lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y);
}

#endif
