// unx-kernel: cs_6_6 main
// Reflection path per pixel (ARCHITECTURE 2.6), one group per 8 x 8 tile. Tiles whose minimum narrow-lobe half-angle
// (M's reflectionLobeTiles, INTERFACES v1.3) is at least the K threshold are all K and exit at once; without that input
// every tile is classified. Per pixel:
//   K  reflectionLobeHalfAngle(r, NoV) >= reflection.cache_lobe_half_angle_min_deg      -> no rays (M evaluates it)
//   M  roughness < reflection.mirror_roughness_max, or the lobe's screen blur < 1 px     -> 1 ray (a job)
//   G  otherwise: blur_px = (d_r / d_view) * lobe * focal_px; spacing s = pow2 clamp(blur / 3, 1, 8); a job at every
//      pixel of the global s-grid (4 rays), the tile's other G pixels interpolate (ReflectionResolve).
// d_r is last frame's reflection hit distance at the pixel (0 before any: s = 1, the conservative choice).
// Jobs are appended with one atomic per wave. The tile's validity texel is set when any pixel is M or G.
//
// P[0] = { depth SRV, gbuffer SRV, lobe tiles SRV (UNX_NONE = none), distance history SRV }
// P[1] = { mode UAV, jobs UAV, counter UAV (uint at 0), reflection UAV }
// P[2] = { K threshold (float radians), mirror roughness max (float), focal length px (float), rows H }
// P[3] = { width, height, planar SRV (raw), planar byte offset }, P[4].x = planar counts UAV; frame constants b1 = main view.
// Mirror-smooth pixels on a planar candidate (inside its rectangle, on its plane, facing along its normal) are counted per
// candidate (the CPU's raster-or-rays choice framesInFlight frames later) and are REFL_PLANAR (no job) when it has a camera.
#include "Passes/Reflection/ReflectionInternal.hlsli"

groupshared uint g_any;
groupshared uint g_planarCounts[64];

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const float kThreshold = asfloat(P[2].x);
    if (P[0].z != UNX_NONE)
    {
        Texture2D<float> lobes = ResourceDescriptorHeap[P[0].z];
        // unorm8 of angle / pi; one quantisation step of margin (INTERFACES v1.3).
        if (lobes.Load(int3(tile, 0)) * 3.14159265 - 3.14159265 / 255.0 >= kThreshold) return;
    }
    if (lane == 0) g_any = 0;
    g_planarCounts[lane] = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint2 size = P[3].xy;
    const uint2 pixel = tile * 8 + local;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].y];
    Texture2D<float> history = ResourceDescriptorHeap[P[0].w];
    RWTexture2D<uint> modes = ResourceDescriptorHeap[P[1].x];
    RWStructuredBuffer<uint> jobs = ResourceDescriptorHeap[P[1].y];
    RWByteAddressBuffer counter = ResourceDescriptorHeap[P[1].z];

    uint mode = REFL_K, spacingLog2 = 0;
    bool job = false;
    if (all(pixel < size))
    {
        const ReflSurface s = reflSurface(depth, gbuffer, pixel);
        const uint2 planar = s.valid && s.roughness <= asfloat(P[2].y) ? reflPlanarCounts(P[3].z, P[3].w) : uint2(0, 0);
        [loop] for (uint k = 0; k < planar.x; ++k)
        {
            const ReflPlanar pl = reflPlanar(P[3].z, P[3].w, k);
            if (any(pixel < pl.rect.xy) || any(pixel >= pl.rect.xy + pl.rect.zw)) continue;
            if (abs(dot(pl.plane.xyz, s.position) + pl.plane.w) > 2e-3 * s.linearDepth + 1e-3 || dot(pl.plane.xyz, s.normal) < 0.999) continue;
            InterlockedAdd(g_planarCounts[k], 1u);
            if (k < planar.y)
            {
                mode = REFL_PLANAR;
                spacingLog2 = k;
            }
            break;
        }
        if (s.valid && mode == REFL_K)
        {
            const float lobe = reflectionLobeHalfAngle(s.roughness, dot(s.normal, s.view));
            if (lobe < kThreshold)
            {
                const float dr = history.Load(int3(pixel, 0));
                const float blur = dr / max(s.linearDepth, 1e-4) * lobe * asfloat(P[2].z);
                if (s.roughness < asfloat(P[2].y) || blur < 1)
                {
                    mode = REFL_M;
                    job = true;
                }
                else
                {
                    mode = REFL_G;
                    spacingLog2 = (uint)clamp(floor(log2(max(blur / 3, 1.0))), 0.0, 3.0);
                    const uint sp = 1u << spacingLog2;
                    job = all((pixel % sp) == sp / 2);  // on the global s-grid (s = 1: every pixel)
                }
            }
        }
    }
    // One atomic per wave for the jobs.
    const uint count = WaveActiveCountBits(job);
    uint base = 0;
    if (WaveIsFirstLane() && count > 0) counter.InterlockedAdd(0, count, base);
    base = WaveReadLaneFirst(base);
    const uint index = job ? base + WavePrefixCountBits(job) : REFL_NO_JOB;
    // Statistics (bytes 4, 8, 12 of the argument buffer): M jobs, G jobs, G pixels.
    const uint mJobs = WaveActiveCountBits(job && mode == REFL_M), gJobs = WaveActiveCountBits(job && mode == REFL_G), gPixels = WaveActiveCountBits(mode == REFL_G);
    if (WaveIsFirstLane())
    {
        if (mJobs) counter.InterlockedAdd(4, mJobs);
        if (gJobs) counter.InterlockedAdd(8, gJobs);
        if (gPixels) counter.InterlockedAdd(12, gPixels);
    }
    if (job) jobs[index] = reflPackPixel(pixel);
    if (all(pixel < size)) modes[pixel] = reflPackMode(mode, spacingLog2, index);
    if (mode != REFL_K) g_any = 1;
    GroupMemoryBarrierWithGroupSync();
    if (g_planarCounts[lane] != 0)
    {
        RWByteAddressBuffer counts = ResourceDescriptorHeap[P[4].x];
        counts.InterlockedAdd(lane * 4, g_planarCounts[lane]);
    }
    if (lane == 0 && g_any != 0)
    {
        RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[1].w];
        reflection[uint2(tile.x, P[2].w + tile.y)] = float4(0, 0, 0, 1);
    }
}
