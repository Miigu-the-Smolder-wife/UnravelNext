// The hair density volume (owner: E; readers: M's hair records, the hair's shadow on other surfaces - HairShadow.hlsl -,
// R's rays - RayTracing/HitHair.hlsli): how much hair lies along a ray inside a body's groom.
// Per frame and body, a grid of cubic cells over the body's bounds (HairSystem.cpp: the rest pose's box under the frame's
// joints, widened by a quarter of the strand length and the follow strands' offsets; at most HAIR_DENSITY_RES cells
// along its longest side) holds
//   rho = sum over the body's drawn segments in the cell of (length x diameter) / cell volume   [1/m]
// - the fibres a ray crosses per metre when it runs across them. LOD keeps it: a body drawn with a fraction of its strands
// draws them wider by 1 / fraction. Two textures (R16_FLOAT, all bodies side by side): the cells, and their means over
// 4 x 4 x 4 cells for long marches and for the cells no march needs to enter.
// The expected number of fibres a ray meets is n = (pi / 4) x integral of rho along the ray (pi / 4: the mean of |sin|
// between the ray and a fibre of any direction - the volume does not keep the fibres' directions). Three marches:
//   hairFibreCount / hairFibreCountWithin   a strand's: from a point inside its own body's hair to the volume's edge (or
//       over at most 'reach' metres: a light inside the box), 'steps' samples, starting half a cell from the point (its
//       own cell's hair is around it, not in front of it). A path of up to 1.5 cells a step reads the cells; a longer one
//       reads the cells over its first steps / 2 cells, one a step - the hair next to the strand, whose edge is the edge
//       of the shadow on it - and the rest in their means, one coarse cell a step (at most 'steps' of them: a longer
//       rest in 'steps' equal steps). (One texture chosen by the whole path's step put every path longer than 1.5 x
//       steps cells - 29 cm of a head's 1.2 cm cells at 16 steps: nearly every path through a groom - into the 4.7 cm
//       means, read 6 cells apart: the shadow's edge on the hair was as coarse as those, with a line where the choice
//       changed.) Samples: 'steps' for a short path, steps / 2 + the rest's coarse cells for a long one.
//   hairFibreCountOthers   the fibres of the other bodies with a block on a strand's path to a light (a beard under the
//       head's hair, the next head), by hairTransmittance's march.
//   hairTransmittance   a surface's that is not hair (HairShadow.hlsl): exp(-n) of every body with a block along the
//       path to a light. Per body in steps of one coarse cell; a step whose mean (trilinear at its middle: 0 means the
//       8 coarse cells around are empty, and the step lies inside them) holds no hair is skipped, the others are read
//       in HAIR_DENSITY_COARSE samples of the cells - the cells' resolution along the whole path at the cost of the
//       hair on it. A path of more than 'steps' coarse cells reads the means alone in 'steps' steps.
//   hairFirstFibre / hairFirstFibreAmong   where a ray first meets a fibre. The fibres met by distance t are Poisson
//       with mean n(t), so the first lies before t with probability 1 - exp(-n(t)). hairFirstFibre: the u-quantile of
//       its place among the rays that meet one in a body, and the probability that a ray does (HAIR_FIRST_STEPS equal
//       steps). hairFirstFibreAmong: a ray's own draw over every body with a block - per body the count at which its
//       first fibre lies is exponential with mean 1 (a draw of its own per body: the bodies' fibres are independent),
//       found by hairTransmittance's march; the nearest of the bodies' places.
// Every march takes its samples at (i + jitter) x step, jitter in [0, 1) (0.5: the midpoint rule). A reader that draws
// the jitter anew per pixel and frame turns what is left of the cells' pattern along the march into noise its temporal
// filter removes (as the reference's voxel traversal: one random offset per ray, converged by its temporal filter).
// Limits: the cell size (bounds / 64: about 1 cm on a head); segments that leave the bounds are not counted; anything that
// is not hair is not in it (opaque geometry is the shadow maps' and the rays'). A strand counts its own body's hair alone
// (hairFibreCount); hairTransmittance and hairFirstFibreAmong count the bodies with a block, the nearest
// HAIR_SHADOW_BODIES at most.
// Parameters (raw): word 0 the frame's bodies (FrameResources::hairBodies' order), 1 the cells per side of a block, 2 the
// fine texture's SRV, 3 the coarse one's, 4..6 the world position the bodies' origins are relative to (floats: the main
// view's camera, FrameResources::hairOrigin), 7 unused; then per body 8 words { origin xyz (m, relative to that position),
// cell size (m), cells x, y, z (0: the body has no block - past shading.hair_density_bodies), block x | y << 16 }; then
// one word, the bodies with a block, and per such body, nearest first, 2 words { its index, its material }.
#ifndef UNX_HAIR_DENSITY_HLSLI
#define UNX_HAIR_DENSITY_HLSLI
#include "Bindless.hlsli"

#define HAIR_DENSITY_HEADER 8u
#define HAIR_DENSITY_BODY_WORDS 8u
#define HAIR_DENSITY_LIST_WORDS 2u  // words per body of the list of bodies with a block
#define HAIR_DENSITY_COARSE 4u    // fine cells per coarse cell, per side
#define HAIR_DENSITY_UNIT (1.0f / 1024)  // 1/m per unit of the accumulation words
#define HAIR_SHADOW_BODIES 64u    // bodies hairTransmittance walks
#define HAIR_FIRST_STEPS 24u      // hairFirstFibre's samples
#define HAIR_FIBRES_PER_DENSITY 0.785398163f  // pi / 4: fibres per unit of the integral of rho

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
// The world position the bodies' origins are relative to.
float3 hairDensityOrigin(ByteAddressBuffer params) { return asfloat(params.Load3(16)); }
// The list of the bodies with a block: its first word's byte offset (the count; entry i: 2 words from 4 + 8 i on).
uint hairDensityList(ByteAddressBuffer params) { return 4 * (HAIR_DENSITY_HEADER + HAIR_DENSITY_BODY_WORDS * params.Load(0)); }

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
// One of the two textures (the cells, or their means) and a point's place in the body's block of it.
struct HairDensityVolume
{
    uint srv;
    float3 origin, base, top, size;
    float scale;
};
HairDensityVolume hairDensityLevel(uint4 header, HairDensityBody b, bool coarse)
{
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

// A strand's march. paramsSrv: FrameResources::hairDensityParams (UNX_NONE: no volume). p: the point, relative to the
// volume's origin; d: unit. A body without a block (or no volume): -1 - the reader's rule for it.
float hairFibreCountWithin(uint paramsSrv, uint body, float3 p, float3 d, float reach, uint steps, float jitter = 0.5f)
{
    if (paramsSrv == 0xFFFFFFFFu) return -1;
    ByteAddressBuffer params = ResourceDescriptorHeap[paramsSrv];
    const uint4 header = params.Load4(0);
    if (body >= header.x) return -1;
    const HairDensityBody b = hairDensityBody(params, body);
    if (b.cells.x == 0) return -1;  // (a body past shading.hair_density_bodies)
    float t0, t1;
    if (!hairDensityRange(b, p, d, 0.5f * b.cell, reach, t0, t1)) return 0;
    const HairDensityVolume fine = hairDensityLevel(header, b, false);
    Texture3D<float> cells = ResourceDescriptorHeap[fine.srv];
    // the cells: the whole path when its steps are at most 1.5 cells, else its first steps / 2 cells (one step: none)
    const bool split = t1 - t0 > 1.5f * steps * b.cell;
    const uint nearSteps = split ? steps / 2 : steps;
    const float dt = split ? b.cell : (t1 - t0) / steps;
    float sum = 0;
    [loop] for (uint i = 0; i < nearSteps; ++i) sum += hairDensityAt(cells, fine, p + d * (t0 + (i + jitter) * dt));
    sum *= dt;
    if (split)
    {
        // the rest in the cells' means, a coarse cell a step (more than 'steps' of them: 'steps' equal steps)
        const float tFar = t0 + nearSteps * dt;
        const uint farSteps = clamp((uint)ceil((t1 - tFar) / (HAIR_DENSITY_COARSE * b.cell)), 1u, max(steps, 1u));
        const float dtFar = (t1 - tFar) / farSteps;
        const HairDensityVolume low = hairDensityLevel(header, b, true);
        Texture3D<float> coarse = ResourceDescriptorHeap[low.srv];
        float far = 0;
        [loop] for (uint k = 0; k < farSteps; ++k) far += hairDensityAt(coarse, low, p + d * (tFar + (k + jitter) * dtFar));
        sum += far * dtFar;
    }
    return HAIR_FIBRES_PER_DENSITY * sum;
}
float hairFibreCount(uint paramsSrv, uint body, float3 p, float3 d, uint steps, float jitter = 0.5f)
{
    return hairFibreCountWithin(paramsSrv, body, p, d, 3.0e38f, steps, jitter);
}

// hairTransmittance's march through one body: the fibres on p + t d (d unit) from tStart to 'reach', or - with target
// > 0 - the distance at which their count reaches 'target' (-1: not within 'reach'; the count grows linearly inside a
// sample). The steps are laid from the ray's entry to its exit of the body's box whatever 'reach' is: two readers of one
// ray with different reaches (a screen trace's hit and the world ray's, RayTracing/HitHair.hlsli) take the same samples
// and find the same place.
float hairDensityAcross(ByteAddressBuffer params, uint4 header, uint body, float3 p, float3 d, float tStart, float reach, uint steps, float jitter, float target)
{
    const bool find = target > 0;
    const HairDensityBody b = hairDensityBody(params, body);
    float t0, tExit;
    if (b.cells.x == 0 || !hairDensityRange(b, p, d, tStart, 3.0e38f, t0, tExit) || !(reach > t0)) return find ? -1 : 0;
    const float t1 = min(tExit, reach);
    const float coarseCell = HAIR_DENSITY_COARSE * b.cell;
    const uint count = clamp((uint)ceil((tExit - t0) / coarseCell), 1u, max(steps, 1u));
    const float dt = (tExit - t0) / count;
    const bool refine = dt <= 1.001f * coarseCell;  // (a longer step does not lie inside the cells its middle reads: the means alone)
    const HairDensityVolume fine = hairDensityLevel(header, b, false), low = hairDensityLevel(header, b, true);
    Texture3D<float> cells = ResourceDescriptorHeap[fine.srv];
    Texture3D<float> coarse = ResourceDescriptorHeap[low.srv];
    const uint parts = refine ? HAIR_DENSITY_COARSE : 1u;
    const float part = dt / parts;
    float fibres = 0;
    [loop] for (uint i = 0; i < count; ++i)
    {
        const float ta = t0 + i * dt;
        if (ta >= t1) break;
        const float mean = hairDensityAt(coarse, low, p + d * (ta + (refine ? 0.5f : jitter) * dt));
        if (!(mean > 0)) continue;
        [loop] for (uint k = 0; k < parts; ++k)
        {
            const float tp = ta + k * part;
            if (tp >= t1) break;
            const float rho = refine ? hairDensityAt(cells, fine, p + d * (tp + jitter * part)) : mean;
            const float add = HAIR_FIBRES_PER_DENSITY * rho * part;  // (the whole part's)
            if (find && fibres + add >= target)
            {
                const float t = tp + (add > 0 ? (target - fibres) / add : 0.5f) * part;
                return t <= t1 ? t : -1;
            }
            fibres += add * min((t1 - tp) / part, 1.0f);
        }
    }
    return find ? -1 : fibres;
}

// What the frame's grooms let through from p towards d (unit) over 'reach' metres: exp(-the fibre counts of the bodies
// with a block), each from half a cell on. No volume: 1.
float hairTransmittance(uint paramsSrv, float3 p, float3 d, float reach, uint steps, float jitter = 0.5f)
{
    if (paramsSrv == 0xFFFFFFFFu) return 1;
    ByteAddressBuffer params = ResourceDescriptorHeap[paramsSrv];
    const uint4 header = params.Load4(0);
    const uint list = hairDensityList(params);
    const uint count = min(params.Load(list), HAIR_SHADOW_BODIES);
    float fibres = 0;
    [loop] for (uint i = 0; i < count; ++i)
    {
        const uint body = params.Load(list + 4 + 4 * HAIR_DENSITY_LIST_WORDS * i);
        if (body >= header.x) continue;
        const float cell = asfloat(params.Load(4 * (HAIR_DENSITY_HEADER + HAIR_DENSITY_BODY_WORDS * body) + 12));
        fibres += hairDensityAcross(params, header, body, p, d, 0.5f * cell, reach, steps, jitter, 0);
    }
    return exp(-fibres);
}

// The fibres of the bodies with a block other than 'body' on p + t d (d unit) over 'reach' metres. No volume: 0.
float hairFibreCountOthers(uint paramsSrv, uint body, float3 p, float3 d, float reach, uint steps, float jitter = 0.5f)
{
    if (paramsSrv == 0xFFFFFFFFu) return 0;
    ByteAddressBuffer params = ResourceDescriptorHeap[paramsSrv];
    const uint4 header = params.Load4(0);
    const uint list = hairDensityList(params);
    const uint count = min(params.Load(list), HAIR_SHADOW_BODIES);
    float fibres = 0;
    [loop] for (uint i = 0; i < count; ++i)
    {
        const uint other = params.Load(list + 4 + 4 * HAIR_DENSITY_LIST_WORDS * i);
        if (other == body || other >= header.x) continue;
        fibres += hairDensityAcross(params, header, other, p, d, 0, reach, steps, jitter, 0);
    }
    return fibres;
}

// One unit number per (seed, k) (PCG's output function on the pair).
float hairDensityUnit(uint seed, uint k)
{
    const uint state = (seed + k * 0x9E3779B9u) * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((word >> 22u) ^ word) * (1.0f / 4294967296.0f);
}
// Where the ray p + t d (d unit, t in [0, reach]) first meets a fibre of any body with a block, for the ray drawn with
// 'seed': the distance (-1: it meets none), the body met and its material. outsideOnly: a body whose box holds p is left
// out - for rays that carry light to a point which counts that body's hair itself (the translucency volume's cells: a
// strand takes the cell's light through its own body's hair, CoverageHair.hlsl hairIndirect).
float hairFirstFibreAmong(uint paramsSrv, float3 p, float3 d, float reach, uint steps, uint seed, out uint body, out uint material, bool outsideOnly = false)
{
    body = material = 0;
    if (paramsSrv == 0xFFFFFFFFu) return -1;
    ByteAddressBuffer params = ResourceDescriptorHeap[paramsSrv];
    const uint4 header = params.Load4(0);
    const uint list = hairDensityList(params);
    const uint count = min(params.Load(list), HAIR_SHADOW_BODIES);
    float nearest = -1;
    [loop] for (uint i = 0; i < count; ++i)
    {
        const uint2 entry = params.Load2(list + 4 + 4 * HAIR_DENSITY_LIST_WORDS * i);
        if (entry.x >= header.x) continue;
        if (outsideOnly)
        {
            const HairDensityBody b = hairDensityBody(params, entry.x);
            if (all(p > b.origin) && all(p < b.origin + float3(b.cells) * b.cell)) continue;
        }
        // the count at which this body's first fibre lies on the ray: exponential, mean 1
        const float target = max(-log(max(1 - hairDensityUnit(seed, i), 1e-30f)), 1e-6f);
        const float t = hairDensityAcross(params, header, entry.x, p, d, 0, nearest >= 0 ? nearest : reach, steps, 0.5f, target);
        if (t >= 0)
        {
            nearest = t;
            body = entry.x;
            material = entry.y;
        }
    }
    return nearest;
}
// The direction out of a body's groom at p: against the gradient of the cells' means (central differences a coarse cell
// apart - the groom's shape, not its strands'); 0 where the means do not change.
float3 hairDensityOutward(uint paramsSrv, uint body, float3 p)
{
    ByteAddressBuffer params = ResourceDescriptorHeap[paramsSrv];
    const uint4 header = params.Load4(0);
    const HairDensityBody b = hairDensityBody(params, body);
    const HairDensityVolume low = hairDensityLevel(header, b, true);
    Texture3D<float> coarse = ResourceDescriptorHeap[low.srv];
    const float h = HAIR_DENSITY_COARSE * b.cell;
    const float3 g = float3(hairDensityAt(coarse, low, p + float3(h, 0, 0)) - hairDensityAt(coarse, low, p - float3(h, 0, 0)),
                            hairDensityAt(coarse, low, p + float3(0, h, 0)) - hairDensityAt(coarse, low, p - float3(0, h, 0)),
                            hairDensityAt(coarse, low, p + float3(0, 0, h)) - hairDensityAt(coarse, low, p - float3(0, 0, h)));
    const float l = length(g);
    return l > 1e-6f ? -g / l : float3(0, 0, 0);
}

// The distance along p + t d (d unit, t in [0, reach]) at which the first fibre of 'body' is met, at quantile u in [0, 1)
// of the rays that meet one; 'met' = the probability that a ray meets one, 1 - exp(-n(reach)) (0: 'reach' is returned).
// The count grows linearly inside each of the HAIR_FIRST_STEPS steps (the cells up to 1.5 cells a step, else their means).
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
    const HairDensityVolume v = hairDensityLevel(header, b, dt > 1.5f * b.cell);
    Texture3D<float> volume = ResourceDescriptorHeap[v.srv];
    float counts[HAIR_FIRST_STEPS];
    float sum = 0;
    uint i;
    [loop] for (i = 0; i < HAIR_FIRST_STEPS; ++i)
    {
        sum += HAIR_FIBRES_PER_DENSITY * dt * hairDensityAt(volume, v, p + d * (t0 + (i + 0.5f) * dt));
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
