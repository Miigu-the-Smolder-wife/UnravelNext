// unx-kernel: cs_6_6 main
// r.card.clear (CardLighting.hlsli): the card lighting's persistent state set to "nothing lit yet" - when it is created
// and when the card set was rebuilt from nothing. One thread per atlas texel.
// P[0] = { direct atlas UAV, indirect atlas UAV, final atlas UAV, trace atlas UAV }
// P[1] = { SH atlas UAV red, green, blue, frames atlas UAV }
// P[2] = { page light UAV (raw), uniform bits UAV (raw), atlas size, page capacity }
#include "Passes/SurfaceCache/CardLighting.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint size = P[2].z;
    if (any(id.xy >= size)) return;
    for (uint k = 0; k < 4; ++k)
    {
        RWTexture2D<float3> atlas = ResourceDescriptorHeap[P[0][k]];
        atlas[id.xy] = 0;
    }
    if (all((id.xy % CL_PROBE_SPACING) == 0))
        for (uint c = 0; c < 3; ++c)
        {
            RWTexture2D<float4> sh = ResourceDescriptorHeap[P[1][c]];
            sh[id.xy / CL_PROBE_SPACING] = 0;
        }
    if (all((id.xy % MC_TILE) == 0))
    {
        RWTexture2D<uint> frames = ResourceDescriptorHeap[P[1].w];
        frames[id.xy / MC_TILE] = 0;
        RWByteAddressBuffer uniformBits = ResourceDescriptorHeap[P[2].y];
        const uint at = (id.x / MC_TILE + id.y / MC_TILE * (size / MC_TILE)) * CL_UNIFORM_BYTES;
        uniformBits.Store4(at, uint4(0, 0, 0, 0));
        uniformBits.Store4(at + 16, uint4(0, 0, 0, 0));
    }
    const uint page = id.x + id.y * size;
    if (page < P[2].w)
    {
        RWByteAddressBuffer pageLight = ResourceDescriptorHeap[P[2].x];
        pageLight.Store4(page * CL_PAGE_LIGHT_BYTES, uint4(0, 0, 0, 0));
    }
}
