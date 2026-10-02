// The Lumen translucency volume (lumen.translucency_volume; LumenTranslucencyVolume.cpp): indirect light at points that
// are not opaque surfaces of the view - fog and air, particles, water, glass, the far side of thin leaves. The structure
// and default numbers follow Unreal Engine's (ue6-main LumenTranslucencyVolumeLighting.cpp / .usf, ...HardwareRayTracing
// .usf, read 2026-10-02); the code is ours.
// A view-aligned grid of cells 32 pixels wide and deep on an exponential scale (cell z covers view depths
// 2^(z/4) - 1 .. 2^((z+1)/4) - 1 m, 26 slices to 80 m). Per frame, per cell the view can see into:
//   mark       the 8 radiance-cache probes around the cell (the cell's rays end in the cache);
//   trace      3 x 3 rays over the sphere (equal-area map, the texel jittered per frame) from the cell's centre (jittered
//              per frame, kept in front of the depth buffer): a hit reads the mesh cards' final lighting, a ray that
//              reaches the radiance cache's coverage distance reads the cache, a miss the sky; clamped to 20 exposed units;
//   filter     each ray texel averaged with the same texel of the 3 cells either side, along x, then y, then z;
//   integrate  the 9 rays projected on 4 SH coefficients per colour and blended with the cell's history at the cell's
//              place in the previous view (history weight 0.9).
// Stored: 'ambient' RGBA16F = the SH band 0 coefficient per colour (nits x LTV_SCALE), 'directional' RGBA16F = band 1,
// luminance-weighted (3 coefficients; the reader gives each colour its share by the ambient's hue).
// Readers: ltvIrradiance (the cosine-weighted light on a surface or a medium's side) and ltvRadiance (incident radiance
// from a direction: 2-band, so a wide lobe's worth of detail).
#ifndef UNX_LUMEN_TRANSLUCENCY_VOLUME_HLSLI
#define UNX_LUMEN_TRANSLUCENCY_VOLUME_HLSLI
#include "Bindless.hlsli"

#define LTV_SCALE (1.0 / 64.0)
#define LTV_TRACE_RES 3u          // rays per cell along one side of its sphere map (TracingOctahedronResolution)
#define LTV_PIXEL_SIZE 32u        // GridPixelSize at the reference height (lumen.translucency_volume_grid_reference_height):
                                  // a kernel of the volume takes the frame's from ltvPixelSize()
#define LTV_Z_SCALE 4.0           // GridDistributionZScale: slices per doubling of (depth + offset)
#define LTV_Z_OFFSET 1.0          // GridDistributionLogZOffset
#define LTV_Z_PER_METRE 1.0       // GridDistributionLogZScale (0.01 per cm)
#define LTV_PI 3.14159265358979

// The frame's parameters (a raw buffer; FrameResources::translucencyGiParams).
struct LtvParams
{
    row_major float4x4 worldToClip;  // the view the grid is aligned to (this frame's)
    uint gridX, gridY, gridZ, frame;
    uint ambientSrv, directionalSrv;
    float2 uvScale;  // view size / (grid x 32): the grid's cells are 32 pixels, so its last row and column overhang the view
};
LtvParams ltvParams(uint srv)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    return b.Load<LtvParams>(0);
}

float ltvDepthOfSlice(float slice) { return (exp2(slice / LTV_Z_SCALE) - LTV_Z_OFFSET) / LTV_Z_PER_METRE; }
float ltvSliceOfDepth(float depth) { return log2(max(depth, 0.0) * LTV_Z_PER_METRE + LTV_Z_OFFSET) * LTV_Z_SCALE; }

// A world position's place in the grid: uv over the view (0..1, y down) and the continuous slice; w = the view depth.
float4 ltvGridPosition(row_major float4x4 worldToClip, float3 worldPosition)
{
    const float4 clip = mul(worldToClip, float4(worldPosition, 1));
    const float w = max(clip.w, 1e-6);  // (every projection of this renderer has clip.w = view depth)
    return float4(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5, ltvSliceOfDepth(clip.w), clip.w);
}

float ltvLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

struct LtvSh  // band 0 per colour, band 1 (y, z, x order) luminance-weighted
{
    float3 ambient;
    float3 directional;
};
LtvSh ltvSample(LtvParams p, float3 worldPosition)
{
    const float4 g = ltvGridPosition(p.worldToClip, worldPosition);
    // (outside the view or past the last slice: the nearest cell)
    const float3 uvw = float3(saturate(g.xy) * p.uvScale, clamp(g.z / float(p.gridZ), 0.5 / float(p.gridZ), 1.0 - 0.5 / float(p.gridZ)));
    Texture3D<float4> ambient = ResourceDescriptorHeap[p.ambientSrv];
    Texture3D<float4> directional = ResourceDescriptorHeap[p.directionalSrv];
    LtvSh s;
    s.ambient = ambient.SampleLevel(g_linearClamp, uvw, 0).rgb / LTV_SCALE;
    s.directional = directional.SampleLevel(g_linearClamp, uvw, 0).rgb / LTV_SCALE;
    return s;
}

// The same for a reader that must tell an empty cell from a dark one (LumenHitIndirect.hlsli): the light of the lit
// cells under the sample alone - the values over the ambient's alpha, which the volume blends exactly as it blends the
// light. lit = that alpha: the share of the sample that is traced light; 0 (and no light) outside the view's grid,
// behind the camera, past the last slice and where no cell under the sample was ever seen into.
LtvSh ltvSampleLit(LtvParams p, float3 worldPosition, out float lit)
{
    LtvSh s;
    s.ambient = s.directional = 0;
    lit = 0;
    const float4 g = ltvGridPosition(p.worldToClip, worldPosition);
    if (!(g.w > 1e-5) || any(g.xy < 0) || any(g.xy > 1) || g.z >= float(p.gridZ)) return s;
    const float3 uvw = float3(g.xy * p.uvScale, clamp(g.z / float(p.gridZ), 0.5 / float(p.gridZ), 1.0 - 0.5 / float(p.gridZ)));
    Texture3D<float4> ambient = ResourceDescriptorHeap[p.ambientSrv];
    Texture3D<float4> directional = ResourceDescriptorHeap[p.directionalSrv];
    const float4 a = ambient.SampleLevel(g_linearClamp, uvw, 0);
    if (!(a.a > 1e-3)) return s;
    lit = saturate(a.a);
    s.ambient = a.rgb / (LTV_SCALE * a.a);
    s.directional = directional.SampleLevel(g_linearClamp, uvw, 0).rgb / (LTV_SCALE * a.a);
    return s;
}

// Irradiance (lux) on a surface of normal n at the position: the SH's cosine convolution (band 0 x pi, band 1 x 2 pi / 3).
float3 ltvIrradianceOf(LtvSh s, float3 n)
{
    const float3 hue = s.ambient / max(ltvLuminance(s.ambient), 1e-12);
    const float band1 = 0.488603 * dot(s.directional, float3(n.y, n.z, n.x));
    return max(LTV_PI * 0.282095 * s.ambient + (2.0 * LTV_PI / 3.0) * band1 * hue, 0.0);
}
// Incident radiance (nits) from a direction: the SH itself. Mean radiance over the sphere: ltvRadianceMean.
float3 ltvRadianceOf(LtvSh s, float3 d)
{
    const float3 hue = s.ambient / max(ltvLuminance(s.ambient), 1e-12);
    return max(0.282095 * s.ambient + 0.488603 * dot(s.directional, float3(d.y, d.z, d.x)) * hue, 0.0);
}
float3 ltvRadianceMeanOf(LtvSh s) { return 0.282095 * s.ambient; }
// The incident radiance integrated with a normalised phase function of asymmetry g (Henyey-Greenstein: its SH band l
// is g^l, so band 0 x 1 and band 1 x g) - what a medium scatters per unit of scattering coefficient and length; for an
// isotropic phase function the mean radiance. d: the direction paired with a light's direction in phase(dot(toLight, d), g).
float3 ltvInscatterOf(LtvSh s, float3 d, float g)
{
    const float3 hue = s.ambient / max(ltvLuminance(s.ambient), 1e-12);
    return max(0.282095 * s.ambient + g * 0.488603 * dot(s.directional, float3(d.y, d.z, d.x)) * hue, 0.0);
}

// ltvSrv: FrameResources::translucencyGiParams (UNX_NONE: no volume - 0).
float3 ltvIrradiance(uint ltvSrv, float3 worldPosition, float3 n)
{
    if (ltvSrv == 0xFFFFFFFFu) return 0;
    return ltvIrradianceOf(ltvSample(ltvParams(ltvSrv), worldPosition), n);
}
float3 ltvRadiance(uint ltvSrv, float3 worldPosition, float3 d)
{
    if (ltvSrv == 0xFFFFFFFFu) return 0;
    return ltvRadianceOf(ltvSample(ltvParams(ltvSrv), worldPosition), d);
}
float3 ltvRadianceMean(uint ltvSrv, float3 worldPosition)
{
    if (ltvSrv == 0xFFFFFFFFu) return 0;
    return ltvRadianceMeanOf(ltvSample(ltvParams(ltvSrv), worldPosition));
}
float3 ltvInscatter(uint ltvSrv, float3 worldPosition, float3 d, float g)
{
    if (ltvSrv == 0xFFFFFFFFu) return 0;
    return ltvInscatterOf(ltvSample(ltvParams(ltvSrv), worldPosition), d, g);
}

#endif
