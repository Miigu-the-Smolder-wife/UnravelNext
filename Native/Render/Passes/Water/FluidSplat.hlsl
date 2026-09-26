// unx-kernel: cs_6_6 main
// Fluid surface: each particle adds its quadratic B-spline weights (and weight x velocity) to its 3^3 nodes in fixed
// point (integer atomics: the sums do not depend on the order).
#include "FluidSurface.hlsli"
#include "WaterLinear.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint i = waterLinear(group, thread, 256);
    if (i >= fsCount()) return;
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[4].x];
    RWByteAddressBuffer density = ResourceDescriptorHeap[P[4].z];
    float3 q = fsParticle(i);
    bool moving = fsHasVelocity(); float3 velocity = moving ? fsVelocity(i) : float3(0, 0, 0);
    int3 base = (int3)floor(q - 0.5); float3 f = q - (float3)base;
    float3 wx, wy, wz; fsWeights(f.x, wx); fsWeights(f.y, wy); fsWeights(f.z, wz);
    for (int z = 0; z < 3; ++z) for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x)
    {
        int3 j = base + int3(x, y, z), b = j >> 3;
        if (!fsInside(b)) continue;
        uint slot = table.Load(4 * fsBlockIndex(b)); if (slot == 0) continue;
        int3 l = j - b * 8;
        uint at = 16 * ((slot - 1) * FS_BLOCK_NODES + ((uint)l.z * 8 + (uint)l.y) * 8 + (uint)l.x);
        float w = wx[x] * wy[y] * wz[z];
        density.InterlockedAdd(at, (uint)round(w * FS_SCALE));
        if (moving)
        {
            int3 m = (int3)round(w * velocity * FS_MOMENTUM);
            density.InterlockedAdd(at + 4, (uint)m.x); density.InterlockedAdd(at + 8, (uint)m.y); density.InterlockedAdd(at + 12, (uint)m.z);
        }
    }
}
