// Coverage records' direct light from a tile x depth-interval FAR field (RENDERER_REDESIGN_V2 14.1c, L2c; owner A):
// per listed coverage tile (V's list), the records' depth span is cut into K <= 16 intervals of equal ratio; for each
// interval the froxel list of its representative point (the tile-centre ray at the interval's middle) is classified
// with LightNearFar.hlsli conditions 1-2 (interpolation and point equivalence over the interval's sphere: lateral 8 px,
// depth the interval; every shadow caster NEAR) and the FAR lights' vector irradiance at that point is summed per
// direction bin (26 cones: the 3 x 3 x 3 cube directions, half-angle bound 30 degrees, every Voronoi cell's radius is
// under 27 degrees). A record of the interval then takes, per bin, "above" (n . c_b > sin 30 + 0.2: every light of the
// bin above its horizon with margin -> the bin's E, exact), "below" (< -(sin 30 + 0.2): 0; the Foliage back side reads
// it with -n), or "straddling" (its lights evaluated one by one, exact). NEAR lights are evaluated as before.
// Written by CoverageTileLights.hlsl, read by CoverageShade.hlsli (covShadeFragment) through g_covListed.
// Field per listed tile, COV_TL_TILE_BYTES:
//   0    header: flags (bit 0 valid), K, zMin bits, ln(ratio) bits
//   16   sliceOf[16] (bytes: the froxel slice of each interval's representative)
//   32   NEAR mask per interval (uint2 x 16: bit = entry position in the slice's froxel list; a clear bit = FAR)
//   160  bin id per interval and entry (16 x 64 bytes)
//   1184 E per interval and bin (16 x 26 x float3, 312 B per interval)
#ifndef UNX_COVERAGE_TILE_LIGHTS_HLSLI
#define UNX_COVERAGE_TILE_LIGHTS_HLSLI

#define COV_TL_INTERVALS 16u
#define COV_TL_BINS 26u
#define COV_TL_TILE_BYTES 6176u
#define COV_TL_SLICES_OFFSET 16u
#define COV_TL_MASK_OFFSET 32u
#define COV_TL_BINID_OFFSET 160u
#define COV_TL_E_OFFSET 1184u
#define COV_TL_E_INTERVAL_BYTES 312u
#define COV_TL_SIN_HALF 0.5     // sin 30 degrees: the bins' half-angle bound
#define COV_TL_MARGIN 0.2       // the horizon margin of 14.1 (NF_HORIZON_MARGIN)
#define COV_TL_RATIO 1.026      // interval depth ratio: (dz / 2 / d)^2 x 6 <= 1e-3 for a light at the interval's distance

// Bin b = the 26 nonzero (x, y, z) in {-1, 0, 1}^3 in lexicographic order, normalised.
float3 covTlBinDir(uint b)
{
    const uint i = b < 13 ? b : b + 1;  // skip (0, 0, 0) at index 13
    const int3 q = int3(i / 9, (i / 3) % 3, i % 3) - 1;
    return normalize(float3(q));
}
// The bin of direction d (unit): the nearest centre.
uint covTlBinOf(float3 d)
{
    uint best = 0;
    float bestDot = -2;
    [loop] for (uint b = 0; b < COV_TL_BINS; ++b)
    {
        const float t = dot(d, covTlBinDir(b));
        if (t > bestDot) { bestDot = t; best = b; }
    }
    return best;
}

struct CovTlHeader
{
    uint flags, K;
    float zMin, lnRatio;
};
CovTlHeader covTlHeader(ByteAddressBuffer b, uint tileBase)
{
    const uint4 w = b.Load4(tileBase);
    CovTlHeader h;
    h.flags = w.x;
    h.K = w.y;
    h.zMin = asfloat(w.z);
    h.lnRatio = asfloat(w.w);
    return h;
}
uint covTlSliceOf(ByteAddressBuffer b, uint tileBase, uint k) { return (b.Load(tileBase + COV_TL_SLICES_OFFSET + (k & ~3u)) >> (8 * (k & 3))) & 0xFFu; }
uint2 covTlMask(ByteAddressBuffer b, uint tileBase, uint k) { return b.Load2(tileBase + COV_TL_MASK_OFFSET + k * 8); }
uint covTlBinId(ByteAddressBuffer b, uint tileBase, uint k, uint i) { return (b.Load(tileBase + COV_TL_BINID_OFFSET + k * 64 + (i & ~3u)) >> (8 * (i & 3))) & 0xFFu; }
// E of bin 'bin' of interval k.
float3 covTlE(ByteAddressBuffer b, uint tileBase, uint k, uint bin)
{
    return asfloat(b.Load3(tileBase + COV_TL_E_OFFSET + k * COV_TL_E_INTERVAL_BYTES + bin * 12));
}

#endif
