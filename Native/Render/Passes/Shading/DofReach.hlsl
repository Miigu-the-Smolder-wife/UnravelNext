// unx-kernel: cs_6_6 main
// Depth of field (A5, FEATURES_GAME 4.1; DepthOfField.cpp): per 32 px tile and radius octave, the largest |rho| of that
// octave's pixels whose disk reaches the tile (the shortest distance between the two tiles' rectangles <= that tile's
// largest |rho|), -1 when none does. Octave c's radii are <= 4 x 2^c px, so it looks 1 + ceil(2^c / 8) tiles around
// (17 for octave 7): a fixed bound per octave (1,856 tile reads per tile over the octaves).
// P[0] = { tile maxima SRV (raw, |rho| + 1 or 0 per tile and octave), reach UAV (raw, 8 floats per tile), tilesX, tilesY }
#include "Bindless.hlsli"
#include "Passes/Shading/DofCommon.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const int2 tiles = int2(P[0].zw);
    if (any((int2)id >= tiles)) return;
    ByteAddressBuffer maxima = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer reach = ResourceDescriptorHeap[P[0].y];
    for (uint c = 0; c < DOF_CATEGORIES; ++c)
    {
        const int k = 1 + (int)(((1u << c) + 7u) / 8u);
        float best = -1;
        for (int dy = -k; dy <= k; ++dy)
            for (int dx = -k; dx <= k; ++dx)
            {
                const int2 t = (int2)id + int2(dx, dy);
                if (any(t < 0) || any(t >= tiles)) continue;
                const float m = asfloat(maxima.Load(4 * ((t.y * tiles.x + t.x) * DOF_CATEGORIES + c)));
                if (m == 0) continue;
                const float rho = m - 1.0f;
                const float2 gap = (float)DOF_TILE * max(abs(float2(dx, dy)) - 1.0f, 0.0f);
                if (length(gap) <= rho) best = max(best, rho);
            }
        reach.Store(4 * ((id.y * tiles.x + id.x) * DOF_CATEGORIES + c), asuint(best));
    }
}
