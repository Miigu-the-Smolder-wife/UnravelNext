// The hair density volume (owner: E; readers: M's hair records, the hair's shadow on other surfaces - HairShadow.hlsl):
// how much hair lies along a ray inside a body's groom.
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
// around it, not in front of it). hairFibreCountWithin: the same over at most 'reach' metres (a light inside the box).
// hairTransmittance: exp(-n) summed over every body with a block - what the frame's grooms let through between a point
// that is not hair and a light.
// hairFirstFibre: where a ray first meets a fibre. The fibres met by distance t are Poisson with mean n(t), so the first
// lies before t with probability 1 - exp(-n(t)); the function returns the u-quantile of its place among the rays that
// meet one (and the probability that a ray does).
// Limits: the cell size (bounds / 64: about 1 cm on a head); segments that leave the bounds are not counted; anything that
// is not hair is not in it (opaque geometry is the shadow maps' and the rays'). A strand counts its own body's hair alone
// (hairFibreCount); hairTransmittance counts the bodies with a block, the nearest HAIR_SHADOW_BODIES at most.
// Parameters (raw): word 0 the frame's bodies (FrameResources::hairBodies' order), 1 the cells per side of a block, 2 the
// fine texture's SRV, 3 the coarse one's; then per body 8 words { origin xyz (m, relative to the main view's camera:
// FrameResources::hairOrigin), cell size (m), cells x, y, z (0: the body has no block - past
// shading.hair_density_bodies), block x | y << 16 }; then one word, the bodies with a block, and their indices, nearest
// first.
#ifndef UNX_HAIR_DENSITY_HLSLI
#define UNX_HAIR_DENSITY_HLSLI
#include "Bindless.hlsli"

#define HAIR_DENSITY_HEADER 4u
#define HAIR_DENSITY_BODY_WORDS 8u
#define HAIR_DENSITY_COARSE 4u    // fine cells per coarse cell, per side
#define HAIR_DENSITY_UNIT (1.0f / 1024)  // 1/m per unit of the accumulation words
#define HAIR_SHADOW_BODIES 64u    // bodies hairTransmittance walks
#define HAIR_FIRST_STEPS 24u      // hairFirstFibre's samples

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

// The part of the ray p + t d (d unit) inside the body's box, from tStart on and within 'reach': false when there is none.
bool hairDensityRange(HairDensityBody b, float3 p, float3 d, float tStart, float reach, out float t0, out float t1)
{
    const float3 lo = b.origin, hi = b.origin + float3(b.cells) * b.cell;
    const float3 inv = 1 / float3(abs(d.x) > 1e-6f ? d.x : 1e-6f, abs(d.y) > 1e-6f ? d.y : 1e-6f, abs(d.z) > 1e-6f ? d.z : 1e-6f);
    const float3 ta = (lo - p) * inv, tb = (hi - p) * inv;
    const float3 tn = min(ta, tb), tf = max(ta, tb);
    t0 = max(max(tn.x, max(tn.y, tn.z)), tStart);
    t1 = min(min(tf.x, min(tf.y, tf.z)), reach);
    return t1 > t0;
}
// The texture a march of step dt reads (the cells, or their means when the step is longer than 1.5 cells) and a point's
// place in the body's block of it.
struct HairDensityVolume
{
    uint srv;
    float3 origin, base, top, size;
    float scale;
};
HairDensityVolume hairDensityVolume(uint4 header, HairDensityBody b, float dt)
{
    const bool coarse = dt > 1.5f * b.cell;
    HairDensityVolume v;
    v.srv = coarse ? header.w : header.z;
    Texture3D<float> volume = ResourceDescriptorHeap[v.srv];  // (its size; the march makes its own handle once)
    const float res = float(header.y) / (coarse ? HAIR_DENSITY_COARSE : 1u);  // texels of a block per side
    v.scale = (coarse ? 1.0f / HAIR_DENSITY_COARSE : 1.0f) / b.cell;
    volume.GetDimensions(v.size.x, v.size.y, v.size.z);
    v.origin = b.origin;
    v.base = float3(float2(b.block) * res, 0);
    v.top = float3(b.cells) * (coarse ? 1.0f / HAIR_DENSITY_COARSE : 1.0f);
    return v;
}
float hairDensityAt(Texture3D<float> volume, HairDensityVolume v, float3 x)
{
    // (clamped half a texel inside the body's block: its neighbours in the texture are other bodies)
    const float3 q = clamp((x - v.origin) * v.scale, 0.5f, max(v.top - 0.5f, 0.5f));
    return volume.SampleLevel(g_linearClamp, (v.base + q) / v.size, 0);
}

// paramsSrv: FrameResources::hairDensityParams (UNX_NONE: no volume). p: the point, relative to the volume's origin
// camera; d: unit. A body without a block (or no volume): -1 - the reader's rule for it.
float hairFibreCountWithin(uint paramsSrv, uint body, float3 p, float3 d, float reach, uint steps)
{
    if (paramsSrv == 0xFFFFFFFFu) return -1;
    ByteAddressBuffer params = ResourceDescriptorHeap[paramsSrv];
    const uint4 header = params.Load4(0);
    if (body >= header.x) return -1;
    const HairDensityBody b = hairDensityBody(params, body);
    if (b.cells.x == 0) return -1;  // (a body past shading.hair_density_bodies)
    float t0, t1;
    if (!hairDensityRange(b, p, d, 0.5f * b.cell, reach, t0, t1)) return 0;
    const float dt = (t1 - t0) / steps;
    const HairDensityVolume v = hairDensityVolume(header, b, dt);
    Texture3D<float> volume = ResourceDescriptorHeap[v.srv];
    float sum = 0;
    [loop] for (uint i = 0; i < steps; ++i) sum += hairDensityAt(volume, v, p + d * (t0 + (i + 0.5f) * dt));
    return 0.785398163f * sum * dt;
}
float hairFibreCount(uint paramsSrv, uint body, float3 p, float3 d, uint steps) { return hairFibreCountWithin(paramsSrv, body, p, d, 3.0e38f, steps); }

// What the frame's grooms let through from p towards d over 'reach' metres: exp(-the fibre counts of the bodies with a
// block). No volume: 1.
float hairTransmittance(uint paramsSrv, float3 p, float3 d, float reach, uint steps)
{
    if (paramsSrv == 0xFFFFFFFFu) return 1;
    ByteAddressBuffer params = ResourceDescriptorHeap[paramsSrv];
    const uint list = 4 * (HAIR_DENSITY_HEADER + HAIR_DENSITY_BODY_WORDS * params.Load(0));
    const uint count = min(params.Load(list), HAIR_SHADOW_BODIES);
    float fibres = 0;
    [loop] for (uint i = 0; i < count; ++i) fibres += max(hairFibreCountWithin(paramsSrv, params.Load(list + 4 + 4 * i), p, d, reach, steps), 0.0f);
    return exp(-fibres);
}

// The distance along p + t d (d unit, t in [0, reach]) at which the first fibre of 'body' is met, at quantile u in [0, 1)
// of the rays that meet one; 'met' = the probability that a ray meets one, 1 - exp(-n(reach)) (0: 'reach' is returned).
// The count grows linearly inside each of the HAIR_FIRST_STEPS steps.
float hairFirstFibre(uint paramsSrv, uint body, float3 p, float3 d, float reach, float u, out float met)
{
    met = 0;
    if (paramsSrv == 0xFFFFFFFFu) return reach;
    ByteAddressBuffer params = ResourceDescriptorHeap[paramsSrv];
    const uint4 header = params.Load4(0);
    if (body >= header.x) return reach;
    const HairDensityBody b = hairDensityBody(params, body);
    if (b.cells.x == 0) return reach;
    float t0, t1;
    if (!hairDensityRange(b, p, d, 0, reach, t0, t1)) return reach;
    const float dt = (t1 - t0) / HAIR_FIRST_STEPS;
    const HairDensityVolume v = hairDensityVolume(header, b, dt);
    Texture3D<float> volume = ResourceDescriptorHeap[v.srv];
    float counts[HAIR_FIRST_STEPS];
    float sum = 0;
    uint i;
    [loop] for (i = 0; i < HAIR_FIRST_STEPS; ++i)
    {
        sum += 0.785398163f * dt * hairDensityAt(volume, v, p + d * (t0 + (i + 0.5f) * dt));
        counts[i] = sum;
    }
    met = 1 - exp(-sum);
    if (!(met > 0)) return reach;
    const float target = -log(max(1 - u * met, 1e-30f));  // the count at which the first fibre has been met with probability u x met
    float before = 0;
    [loop] for (i = 0; i < HAIR_FIRST_STEPS; ++i)
    {
        if (counts[i] >= target) return t0 + (i + (counts[i] > before ? (target - before) / (counts[i] - before) : 0.5f)) * dt;
        before = counts[i];
    }
    return t1;
}

#endif
