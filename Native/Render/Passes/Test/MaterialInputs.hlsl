// unx-kernel: cs_6_6 main
// Core test kernel (material inputs; Scene.hlsli GpuMaterialInputs, Passes/Material/MaterialInputs.hlsli): for
// deterministic points, the records of the scene's materials P[0].z .. P[0].z + P[0].w - 1 as GpuScene packed them and
// what the kernels make of them - the material's uv, a normal map's slope in the mesh's frame, the alpha test's
// threshold at a pixel - and the vertex streams of mesh P[1].x's vertices (P[1].y of them), through FrameConstants.
// Per point 16 words into a raw UAV: the uv (2), the slope (2), its transformed uv (2), its transformed slope (2), the
// record's flags and index (2), the detail scale (2), the pixel (2: its threshold in the next word... see below).
//   words 0..1 uv, 2..3 slope, 4..5 materialUv, 6..7 mInputSlope, 8 flags, 9 GpuMaterial::inputs, 10..11 detail scale,
//   12 alpha threshold at pixel (i % 61, i % 53), 13 the mesh's attribute base, 14 vertex (i % P[1].y)'s uv1.x, 15 its
//   colour as RGBA8
//   P[0].x output UAV (raw), P[0].y point count
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Material/MaterialInputs.hlsli"
#include "Passes/Material/MaterialTextures.hlsli"

float hash01(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (x >> 8) * (1.0 / 16777216.0);
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    const GpuMaterial m = loadMaterial(P[0].z + i % P[0].w);
    const float2 uv = float2(hash01(4 * i + 1), hash01(4 * i + 2)) * 4 - 2;
    const float2 slope = float2(hash01(4 * i + 3), hash01(4 * i + 4)) * 2 - 1;
    const MInputUv iu = mInputUv(m, uv, float2(1, 0), float2(0, 1));
    const float2 slopeMesh = mInputSlope(iu.r, slope);
    const uint base = meshAttributeBase(P[1].x);
    float2 uv1 = 0;
    float4 color = 0;
    if (base != 0) loadVertexAttributes(base, i % P[1].y, uv1, color);
    const uint4 c8 = uint4(round(color * 255.0));
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].x];
    const uint b = 64 * i;
    o.Store4(b, asuint(float4(uv, slope)));
    o.Store4(b + 16, asuint(float4(materialUv(m, uv), slopeMesh)));
    o.Store4(b + 32, uint4(iu.r.flags, m.inputs, asuint(iu.r.detailScaleU), asuint(iu.r.detailScaleV)));
    o.Store4(b + 48, uint4(asuint(materialAlphaThreshold(m, float2(i % 61, i % 53) + 0.5)), base, asuint(uv1.x), c8.x | (c8.y << 8) | (c8.z << 16) | (c8.w << 24)));
}
