// unx-kernel: cs_6_6 main
// A9 anisotropy tests (Passes/Material/Aniso.hlsli; AnisoTests.cpp).
// P[0] = { queries SRV, out UAV, count, mode }
// mode 0 (mirror): 5 x float4 per query - (v local, roughness), (l local, strength), (T, sign), (N, theta), (n, 0);
//        out 4 x float4 - (anisoSpecular with f0 (1, 0.5, 0.04), E_a), (frame t, ok), (t after the word's round trip about
//        octDecode(octEncode(n)), 0), (anisoPdf, D, V, 0).
// mode 1 (V: the frame of the vis-buffer surface): P[1] = { vis id SRV, visible clusters SRV, width, height }; per pixel
//        out 2 x float4 - (t, 1 = surface), (alpha_t', alpha_b' band-limited by the footprint, 0, 0), for n = the
//        normalised interpolated normal (no map) and the material's record.
#include "Bindless.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Material/Aniso.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].z) return;
    RWStructuredBuffer<float4> o = ResourceDescriptorHeap[P[0].y];
    if (P[0].w == 0)
    {
        StructuredBuffer<float4> q = ResourceDescriptorHeap[P[0].x];
        const float4 a = q[5 * i], b = q[5 * i + 1], c = q[5 * i + 2], d = q[5 * i + 3], e = q[5 * i + 4];
        const float3 v = normalize(a.xyz), l = normalize(b.xyz);
        const float2 alpha = anisoAlphas(a.w, b.w);
        const float2 ab = anisoSpecularAlbedo(v, alpha);
        const float3 f = anisoSpecular(float3(1, 0.5, 0.04), v, l, alpha, ab);
        o[4 * i] = float4(f, ab.x + ab.y);
        float3 t, bt;
        float2 rot;
        sincos(d.w, rot.y, rot.x);
        const bool ok = anisoFrame(c.xyz, c.w, d.xyz, rot, normalize(e.xyz), t, bt);
        o[4 * i + 1] = float4(t, ok ? 1 : 0);
        const float3 n = normalize(e.xyz), nd = octDecode(octEncode(n));
        float3 t2, b2;
        float2 al2;
        anisoUnpackWord(anisoPackWord(nd, t, alpha), nd, n, t2, b2, al2);
        o[4 * i + 2] = float4(t2, 0);
        const float3 h = normalize(v + l);
        o[4 * i + 3] = float4(anisoPdf(v, l, alpha), anisoD(h, alpha), anisoV(v, l, alpha), 0);
        return;
    }
    const uint W = P[1].z, H = P[1].w;
    const uint2 pixel = uint2(i % W, i / W);
    Texture2D<uint> vis = ResourceDescriptorHeap[P[1].x];
    const uint id = vis[pixel];
    if (id == VIS_NONE)
    {
        o[2 * i] = 0;
        o[2 * i + 1] = 0;
        return;
    }
    const MSurface s = mSurfaceFromVis(id, P[1].y, float2(pixel) + 0.5);
    const GpuMaterial m = loadMaterial(s.material);
    const AnisoRecord rec = anisoRecordOf(m);
    const float3 n = normalize(s.normal);
    float3 t, b;
    anisoFrameOfSurface(s.tangent, s.tangentSign, s.normal, rec, n, t, b);
    const float2 alpha = anisoBandLimit(anisoAlphas(m.roughness, rec.strength), t, b, s.dndx, s.dndy, 0);
    o[2 * i] = float4(t, 1);
    o[2 * i + 1] = float4(alpha, 0, 0);
}
