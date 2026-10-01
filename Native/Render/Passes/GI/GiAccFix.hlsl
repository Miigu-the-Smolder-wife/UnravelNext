// unx-kernel: cs_6_6 main
// gi.hit_accumulator_frame (GiInternal.hlsli GI_ACC_*): the direct term of every GI ray's hit from its cell's mean over this
// frame's hits (GiAccumulate stored them), not over a window of earlier frames. A window mean read by this frame's hits
// lost energy: the frames in which a cell gets many hits (anchors beside a lamp, in the cell's bright part) are the frames
// whose own mean is high, and the window dilutes it (bath energy audit 0.98 at every cell size [measured]); the same
// frame's mean sums, over the cell's hits, to their point values exactly. GiTrace wrote each ray's record (its cell, the
// reader's factors kA = plain albedo, kB = coated albedo, kC = coated, and its point value); this pass adds
// (kA A + kB B + kC C) - point to the ray's irradiance and texel samples before GiIntegrate.
// P[0] = { cache UAV, rays, samples UAV, records SRV }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint ray : SV_DispatchThreadID)
{
    if (ray >= P[0].y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint4> samples = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer records = ResourceDescriptorHeap[P[0].w];
    const uint4 r0 = records.Load4(ray * 48), r1 = records.Load4(ray * 48 + 16), r2 = records.Load4(ray * 48 + 32);
    const uint cell = r0.x;
    if (cell == 0xFFFFFFFFu) return;
    const GiHeader h = giHeader(b);
    const float3 kA = asfloat(r0.yzw), kB = asfloat(r1.xyz);
    const float kC = asfloat(r1.w);
    const float3 pointValue = asfloat(r2.xyz);
    const uint a = b.Load(GI_ACC_OFFSET) + cell * 48;
    const uint4 w0 = b.Load4(a), w1 = b.Load4(a + 16), w2 = b.Load4(a + 32);
    if (w2.z != h.epoch || !(asfloat(w2.y) > 0)) return;
    const float3 A = asfloat(w0.xyz), B = asfloat(uint3(w0.w, w1.x, w1.y)), C = asfloat(uint3(w1.z, w1.w, w2.x));
    const float3 acc = kA * A + kB * B + kC * C;
    const float3 delta = acc - pointValue;
    if (!all(delta == delta) || any(abs(delta) > 3.0e38)) return;
    uint4 s = samples[ray];
    s.xyz = asuint(asfloat(s.xyz) + delta);
    samples[ray] = s;
    uint4 t = samples[2 * P[0].y + ray];
    t.xyz = asuint(asfloat(t.xyz) + delta);
    samples[2 * P[0].y + ray] = t;
    if ((b.Load(GI_ACC_MODE) & 2u) != 0)
    {
        // energy audit (experiment 32768)
        const float3 Y = float3(0.2126, 0.7152, 0.0722);
        uint64_t prev;
        b.InterlockedAdd64(GI_ACC_AUDIT, (uint64_t)(max(dot(pointValue, Y), 0.0) * 1024), prev);
        b.InterlockedAdd64(GI_ACC_AUDIT + 8, (uint64_t)(max(dot(acc, Y), 0.0) * 1024), prev);
    }
}
