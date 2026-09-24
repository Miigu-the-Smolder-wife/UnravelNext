// unx-kernel: ps_6_6 main
// M tests: stand-in band-A raster pixel shader (FakeVis.hlsli): alpha test at the pixel centre, writes the vis id.
// P[0].z texture table SRV
#include "Passes/Material/Tests/FakeVis.hlsli"

uint main(FakeVisVertex v, FakeVisPrimitive p) : SV_Target0
{
    const GpuMaterial m = loadMaterial(p.material);
    if ((m.classFlags & MATERIAL_ALPHA_TESTED) != 0)
    {
        const MTextureSet ts = mLoadTextureSet(P[0].z, p.material);
        if (ts.baseColor != UNX_NONE)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.baseColor];
            if (t.SampleLevel(g_linearWrap, v.uv, 0).a < m.alphaCutoff) discard;
        }
    }
    return p.visId;
}
