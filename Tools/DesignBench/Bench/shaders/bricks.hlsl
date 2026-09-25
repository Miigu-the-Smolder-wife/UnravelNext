// Bench 2: band C brick DDA (COVERAGE_REDESIGN_KO.md 6.3, 6.4, 7.3).
// A dense grid of 16^3 bricks (Bx x By x Bz) fills a box; brick ids come from a table (the octree/page indirection),
// voxels from a raw buffer (VOXEL_BYTES 1: density byte; 8: density + 7 bytes read as two dwords). Camera rays enter
// the box and take STEPS unit steps (one voxel per step), accumulating transmittance and density-weighted attributes,
// and write a 32 B aggregate record per pixel. ORTHO=1 marches parallel rays from the top face (sun view).
// EntryMapCS builds a 16 x 16 transmittance map per brick for one direction (16 steps per cell).
// P[0] = { width, height, steps, mode(0 camera, 1 ortho) }, P[1] = { brick table SRV, voxels SRV (raw), output UAV, entry maps UAV }
// P[2] = { Bx, By, Bz, brick count }, P[3] = { seed, unused... }
#include "common.hlsli"

#ifndef VOXEL_BYTES
#define VOXEL_BYTES 1
#endif
#ifndef STEPS
#define STEPS 32
#endif
#ifndef OUT_T
#define OUT_T 0  // 1: write only the 4 B transmittance (receiver sun march, revision 1 11.4 (5)); 0: the 32 B aggregate record
#endif

struct Aggregate { float T; float depth; uint sggx[3]; uint albedoMaterial; uint entry01; uint entry2pad; };

uint voxelAddress(uint brick, int3 local) { return brick * (4096u * VOXEL_BYTES) + (uint)((local.z * 256 + local.y * 16 + local.x) * VOXEL_BYTES); }

float densityAt(ByteAddressBuffer voxels, uint brick, int3 local, inout float extra)
{
#if VOXEL_BYTES == 1
    const uint addr = voxelAddress(brick, local);
    const uint word = voxels.Load(addr & ~3u);
    return ((word >> ((addr & 3u) * 8)) & 0xFFu) / 255.0;
#else
    const uint2 w = voxels.Load2(voxelAddress(brick, local));
    extra += (w.y & 0xFFu) / 255.0;  // the SGGX bytes are consumed so the load is not dead
    return (w.x & 0xFFu) / 255.0;
#endif
}

[numthreads(8, 8, 1)]
void MarchCS(uint2 pixel : SV_DispatchThreadID)
{
    const uint W = P[0].x, H = P[0].y;
    if (any(pixel >= uint2(W, H))) return;
    StructuredBuffer<uint> brickTable = ResourceDescriptorHeap[P[1].x];
    ByteAddressBuffer voxels = ResourceDescriptorHeap[P[1].y];
    RWStructuredBuffer<Aggregate> output = ResourceDescriptorHeap[P[1].z];
    const uint3 B = P[2].xyz;
    const float3 box = float3(B * 16);
    float3 o, d;
    if (P[0].w == 0)
    {
        // Camera in front of the box looking +z; 60 deg vertical fov.
        const float2 ndc = (float2(pixel) + 0.5) / float2(W, H) * 2 - 1;
        const float aspect = (float)W / H;
        d = normalize(float3(ndc.x * 0.577 * aspect, -ndc.y * 0.577, 1.0));
        o = float3(box.x * 0.5, box.y * 0.6, -box.z * 0.4);
        // Enter at z = 0 (front face).
        o += d * (-o.z / d.z);
    }
    else if (P[0].w == 1)
    {
        // Sun view: parallel rays down through the top face, texel spacing = box.x / W.
        d = normalize(float3(0.15, -1.0, 0.1));
        o = float3((pixel.x + 0.5) * box.x / W, box.y - 0.001, (pixel.y + 0.5) * box.z / H);
    }
    else
    {
        // Receiver sun march (11.4 (5)): parallel rays *toward* the sun from receivers on a rolling ground height field
        // under the canopy (height 0.5..3.5 voxels), pixel spacing = box.x / W (footprint-matched brick LOD = voxel).
        d = normalize(float3(0.15, 1.0, 0.1));
        const float2 g = (float2(pixel) + 0.5) / float2(W, H);
        const float ground = 0.5 + 1.5 * (1.0 + sin(g.x * 37.0) * cos(g.y * 29.0));
        o = float3(g.x * box.x, ground, g.y * box.z);
    }
    float T = 1;
    float3 pos = o;
    uint brick = 0xFFFFFFFFu, brickIndexPrev = 0xFFFFFFFFu;
    float sumDepth = 0, sumW = 0, extra = 0;
    float firstDepth = -1;
    uint entries = 0;
    [loop] for (uint s = 0; s < STEPS; ++s)
    {
        const int3 vox = int3(floor(pos));
        if (any(vox < 0) || any(vox >= int3(box))) break;
        const uint3 bc = (uint3)vox >> 4;
        const uint brickIndex = (bc.z * B.y + bc.y) * B.x + bc.x;
        if (brickIndex != brickIndexPrev)
        {
            brick = brickTable[brickIndex];  // the indirection (octree node / page table)
            brickIndexPrev = brickIndex;
            ++entries;
        }
        const float sigma = densityAt(voxels, brick, vox & 15, extra);
        const float dT = T * (1 - exp2(-sigma * 2.0));
        if (firstDepth < 0 && dT > 0) firstDepth = s;
        sumDepth += dT * s;
        sumW += dT;
        T -= dT;
        pos += d;
        if (T < 1.0 / 32.0) break;
    }
#if OUT_T
    output[pixel.y * W + pixel.x].T = T + extra * 0.0 + (float)entries * 0.0 + sumW * 0.0;
    return;
#endif
    Aggregate a;
    a.T = T;
    a.depth = sumW > 0 ? sumDepth / sumW : (float)STEPS;
    a.sggx[0] = asuint(extra); a.sggx[1] = entries; a.sggx[2] = asuint(sumW);
    a.albedoMaterial = 0x00FF8040u;
    a.entry01 = f32tof16(firstDepth) | (f32tof16(a.depth) << 16);
    a.entry2pad = 0;
    output[pixel.y * W + pixel.x] = a;
}

// Entry map: per brick, 16 x 16 cells on the face facing direction dir, each marching 16 voxels through the brick.
// One group per brick (256 threads = cells). Output unorm8 transmittance per cell (packed 4 per dword).
// P[0].x = brick count. P[3].xyz = direction quantised (sign bits), P[1].y voxels, P[1].w maps UAV.
[numthreads(256, 1, 1)]
void EntryMapCS(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID)
{
    ByteAddressBuffer voxels = ResourceDescriptorHeap[P[1].y];
    RWByteAddressBuffer maps = ResourceDescriptorHeap[P[1].w];
    const uint brick = gid.x;
    if (brick >= P[2].w) return;
    const int2 cell = int2(tid & 15, tid >> 4);
    float T = 1, extra = 0;
    // Direction: the sun points down (-y) with a small tilt: step x by one voxel every 8 steps.
    [unroll] for (int s = 0; s < 16; ++s)
    {
        const int3 local = int3(clamp(cell.x + s / 8, 0, 15), 15 - s, cell.y);
        const float sigma = densityAt(voxels, brick, local, extra);
        T *= exp2(-sigma * 2.0);
    }
    const uint v = (uint)round(saturate(T + extra * 0.0) * 255.0);
    const uint addr = (brick * 256 + tid);
    maps.InterlockedOr((addr & ~3u), v << ((addr & 3u) * 8));
}
