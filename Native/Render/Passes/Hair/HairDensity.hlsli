// The hair density volume (owner: E; readers: M's hair records): how much hair lies along a ray inside a body's groom.
// Per frame and body, a grid of cubic cells over the body's bounds (HairSystem.cpp: the rest pose's box under the frame's
// joints, widened by a quarter of the strand length and the follow strands' offsets; at most HAIR_DENSITY_RES cells
// along its longest side) holds
//   rho = sum over the body's drawn segments in the cell of (length x diameter) / cell volume   [1/m]
// - the fibres a ray crosses per metre when it runs across them. LOD keeps it: a body drawn with a fraction of its strands
// draws them wider by 1 / fraction. Two textures (R16_FLOAT, all bodies side by side): the cells, and their means over
// 4 x 4 x 4 cells for long marches.
// hairFibreCount: the expected number of fibres a ray meets from a point to the volume's edge,
//   n = (pi / 4) x integral of rho along the ray
// (pi / 4: the mean of |sin| between the ray and a fibre of any direction - the volume does not keep the fibres'
// directions), by the midpoint rule over 'steps' samples, starting half a cell from the point (its own cell's hair is
// around it, not in front of it).
// Limits: the cell size (bounds / 64: about 1 cm on a head); segments that leave the bounds are not counted; another
// body's hair, and anything that is not hair, is not in it (opaque geometry is the shadow maps' and the rays').
// Parameters (raw): word 0 the frame's bodies (FrameResources::hairBodies' order), 1 the cells per side of a block, 2 the
// fine texture's SRV, 3 the coarse one's; then per body 8 words { origin xyz (camera-relative, m), cell size (m), cells
// x, y, z (0: the body has no block - past shading.hair_density_bodies), block x | y << 16 }.
#ifndef UNX_HAIR_DENSITY_HLSLI
#define UNX_HAIR_DENSITY_HLSLI
#include "Bindless.hlsli"

#define HAIR_DENSITY_HEADER 4u
#define HAIR_DENSITY_BODY_WORDS 8u
#define HAIR_DENSITY_COARSE 4u    // fine cells per coarse cell, per side
#define HAIR_DENSITY_UNIT (1.0f / 1024)  // 1/m per unit of the accumulation words

struct HairDensityBody
{
    float3 origin;
    float cell;
    uint3 cells;
    uint2 block;
};
HairDensityBody hairDensityBody(ByteAddressBuffer params, uint body)
{
    const uint at = 4 * (HAIR_DENSITY_HEADER + HAIR_DENSITY_BODY_WORDS * body);
    const uint4 a = params.Load4(at), b = params.Load4(at + 16);
    HairDensityBody o;
    o.origin = asfloat(a.xyz);
    o.cell = asfloat(a.w);
    o.cells = b.xyz;
    o.block = uint2(b.w & 0xFFFFu, b.w >> 16);
    return o;
}

// paramsSrv: FrameResources::hairDensityParams (UNX_NONE: no volume). p: the point, camera-relative; d: unit. A body
// without a block (or no volume): -1 - the reader's rule for it.
float hairFibreCount(uint paramsSrv, uint body, float3 p, float3 d, uint steps)
{
    if (paramsSrv == 0xFFFFFFFFu) return -1;
    ByteAddressBuffer params = ResourceDescriptorHeap[paramsSrv];
    const uint4 header = params.Load4(0);
    if (body >= header.x) return -1;
    const HairDensityBody b = hairDensityBody(params, body);
    if (b.cells.x == 0) return -1;  // (a body past shading.hair_density_bodies)
    // the ray against the body's box
    const float3 lo = b.origin, hi = b.origin + float3(b.cells) * b.cell;
    const float3 inv = 1 / float3(abs(d.x) > 1e-6f ? d.x : 1e-6f, abs(d.y) > 1e-6f ? d.y : 1e-6f, abs(d.z) > 1e-6f ? d.z : 1e-6f);
    const float3 ta = (lo - p) * inv, tb = (hi - p) * inv;
    const float3 tn = min(ta, tb), tf = max(ta, tb);
    const float t0 = max(max(tn.x, max(tn.y, tn.z)), 0.5f * b.cell), t1 = min(tf.x, min(tf.y, tf.z));
    if (!(t1 > t0)) return 0;
    const float dt = (t1 - t0) / steps;
    const bool coarse = dt > 1.5f * b.cell;
    Texture3D<float> volume = ResourceDescriptorHeap[coarse ? header.w : header.z];
    const float res = float(header.y) / (coarse ? HAIR_DENSITY_COARSE : 1u);  // texels of a block per side
    const float scale = (coarse ? 1.0f / HAIR_DENSITY_COARSE : 1.0f) / b.cell;
    float3 size;
    volume.GetDimensions(size.x, size.y, size.z);
    const float3 base = float3(float2(b.block) * res, 0), top = float3(b.cells) * (coarse ? 1.0f / HAIR_DENSITY_COARSE : 1.0f);
    float sum = 0;
    [loop] for (uint i = 0; i < steps; ++i)
    {
        // (clamped half a texel inside the body's block: its neighbours in the texture are other bodies)
        const float3 q = clamp((p + d * (t0 + (i + 0.5f) * dt) - b.origin) * scale, 0.5f, max(top - 0.5f, 0.5f));
        sum += volume.SampleLevel(g_linearClamp, (base + q) / size, 0);
    }
    return 0.785398163f * sum * dt;
}

#endif
