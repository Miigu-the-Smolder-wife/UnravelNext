// The sea's surface for shading (track W, B7; FEATURES_GAME 1.8 B, 1.9): what WaterSurface.hlsli's function needs of a
// pixel of the view-grid ocean (the water layer's samples of COV_OCEAN_SLOT) in place of a stream triangle - the point,
// the normal and what the pixel does not resolve of the surface - and the sea's own terms (its water, its foam).
//   point     the view grid's pixel (FrameResources::waterSurface: rest position x0, view depth): P on the pixel's ray
//             at that depth. (Rest positions are the renderer's coordinates: the frame's origin moves by whole multiples
//             of 1024 m, the cascades' period.)
//   normal    from the cascades' slopes and horizontal-displacement derivatives at x0 (the tangents (1 + dDx/dx, dh/dx,
//             dDx/dz) and (dDx/dz, dh/dz, 1 + dDz/dz)), low-passed over the pixel's footprint on the still plane (the ray
//             differentials carried to the horizontal plane through P; the anisotropic sampler: at a grazing view the
//             footprint is long along the view and the mean slope over it is what the pixel shows).
//   variance  the slopes the footprint removes, into the lobes (the sun's glint, the mirror cone, the mean Fresnel). The
//             cascades hold the JONSWAP spectrum's tail S(k) = alpha / 2 k^-3 (Phillips' saturation range in
//             wavenumber), whose slope variance between two wavenumbers is alpha / 2 ln(k2 / k1): from the footprint's
//             cutoff pi / f to the cascades' finest wavenumber. Past that the capillary waves the cascades do not hold:
//             Cox & Munk's (1954) total mean square slope 0.003 + 0.00512 U less what the spectrum holds up to there.
//             An isotropic lobe of alpha^2 = that variance (a Beckmann lobe's alpha^2 is its total slope variance).
//   Fresnel   over a rough sea the mean reflectance toward the horizon stays far under 1 (the facets the view sees are
//             turned toward it): Bruneton, Neyret & Holzschuch 2010's fit of the mean Fresnel term over the visible
//             slopes, R0 + (1 - R0) (1 - cos)^(5 e^(-2.69 sigma)) / (1 + 22.7 sigma^1.5), sigma^2 the slope variance along
//             the view (half the total); sigma = 0 is Schlick's form of the smooth surface's.
//   water     the open sea where the frame names no material: absorption 0.34 / 0.064 / 0.018 per m at 650 / 550 / 450 nm
//             (pure water, Pope & Fry 1997, with the yellow substance and pigment of clear ocean water, Jerlov IB),
//             scattering 0.020 / 0.028 / 0.036 per m, g 0.75 (one Henyey-Greenstein lobe whose back fraction, 7 %, is the
//             water's molecules' and particles' together: the light the sea returns is its backscatter).
//   foam      the foam clipmap at x0 (Foam.hlsli: the breaking fraction's decaying maximum) and the shore's: where the
//             water over the bed thins out under shoreDepth, more of it the less the surface is stretched (the Jacobian
//             of the horizontal displacement: crests) - an appearance model, as the design has it (1.3 (f), (g)). Foam is
//             a white Lambert layer over the water (albedo 0.85) under the sun and the sky.
#ifndef UNX_WATER_OCEAN_SHADING_HLSLI
#define UNX_WATER_OCEAN_SHADING_HLSLI
#include "Bindless.hlsli"
#include "Passes/Common/Frame.hlsli"
#include "Passes/Water/Foam.hlsli"

// The slot table's ocean block (WaterSurface.cpp: after the planar block; 96 B):
//   +0   surface SRV, displacement SRV, slopes SRV, foam SRV (UNX_NONE: no foam)
//   +16  foam parameters SRV, material (UNX_NONE: the built-in sea water), 1 (the block is there; 0: no sea this frame), 0
//   +32  cascade lengths (m) 0..2, 0
//   +48  0, 0, Phillips' alpha, the spectrum's peak wavenumber (1/m)
//   +64  the cascades' finest wavenumber (1/m), wind speed at 10 m (m/s), mirror rays' reach (m), shore foam depth (m)
//   +80  shore foam amount, foam albedo, 0, 0
#define OCEAN_SHADE_BYTES 96u
#define OCEAN_FOAM_ALBEDO 0.85

struct OceanShade
{
    uint surface, displacement, slopes, foam, foamParams, material;
    bool there;
    float3 lengths;
    float alpha, kPeak, kMax, wind, rayReach, shoreDepth, shoreFoam, foamAlbedo;
};
OceanShade oceanShadeLoad(uint table, uint at)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[table];
    const uint4 r0 = b.Load4(at), r1 = b.Load4(at + 16);
    const float4 r2 = asfloat(b.Load4(at + 32)), r3 = asfloat(b.Load4(at + 48)), r4 = asfloat(b.Load4(at + 64)), r5 = asfloat(b.Load4(at + 80));
    OceanShade o;
    o.surface = r0.x, o.displacement = r0.y, o.slopes = r0.z, o.foam = r0.w;
    o.foamParams = r1.x, o.material = r1.y;
    o.there = r1.z == 1u;
    o.lengths = r2.xyz;
    o.alpha = r3.z, o.kPeak = r3.w;
    o.kMax = r4.x, o.wind = r4.y, o.rayReach = r4.z, o.shoreDepth = r4.w;
    o.shoreFoam = r5.x, o.foamAlbedo = r5.y;
    return o;
}

struct OceanSurfacePoint
{
    float3 P, n;      // the sample (renderer space) and the surface's unit normal, up
    float2 x0;        // its rest position
    float depth;      // view depth
    float variance;   // total slope variance the pixel does not resolve (both axes)
    float footprint;  // the pixel's footprint on the still plane (m; the geometric mean of its sides)
    float jacobian;   // of the horizontal displacement at x0 (1: unstretched; toward 0: a crest folding)
    float3 dPx, dPy;  // the point per pixel, on the horizontal plane through P
};

// D, Dx, Dy: mPixelRay's (D with unit view depth: P = camera + D x view depth).
OceanSurfacePoint oceanSurfacePoint(OceanShade o, uint2 pixel, float3 D, float3 Dx, float3 Dy)
{
    Texture2D<float4> surface = ResourceDescriptorHeap[o.surface];
    const float4 sf = surface[pixel];
    OceanSurfacePoint p;
    p.x0 = sf.xy;
    p.depth = sf.z;
    p.P = g_cameraPosition + D * sf.z;
    // the footprint on the still plane: the ray differentials at the horizontal plane through P (a ray along the plane
    // keeps a finite one: the samplers hold it to their coarsest level)
    const float dy = abs(D.y) > 1e-5 ? D.y : (D.y < 0 ? -1e-5 : 1e-5);
    p.dPx = sf.z * (Dx - D * (Dx.y / dy));
    p.dPy = sf.z * (Dy - D * (Dy.y / dy));
    float2 a = p.dPx.xz, b = p.dPy.xz;
    const float la = length(a), lb = length(b), longest = max(max(la, lb), 1e-6);
    const float held = min(1.0, 256.0 / longest);  // (256 m: the first cascade's quarter)
    a *= held;
    b *= held;
    // the sides the anisotropic sampler filters with: the long one, and a short one not under a sixteenth of it
    const float major = longest * held, area = abs(a.x * b.y - a.y * b.x);
    const float minor = max(area / max(major, 1e-6), major / 16.0);
    p.footprint = sqrt(major * minor);
    Texture2DArray<float4> field = ResourceDescriptorHeap[o.displacement];
    Texture2DArray<float4> slopes = ResourceDescriptorHeap[o.slopes];
    float4 s = 0;   // dh/dx, dh/dz, dDx/dx, dDz/dz
    float shear = 0;  // dDx/dz (= dDz/dx: the horizontal displacement is a gradient field)
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float L = o.lengths[c];
        const float3 uv = float3(p.x0 / L + 0.5 / 512.0, c);
        s += slopes.SampleGrad(g_anisoWrap, uv, a / L, b / L);
        shear += field.SampleGrad(g_anisoWrap, uv, a / L, b / L).w;
    }
    p.n = normalize(cross(float3(shear, s.y, 1 + s.w), float3(1 + s.z, s.x, shear)));
    if (!(p.n.y > 0)) p.n = float3(0, 1, 0);  // (a folded crest's mean, or a sample that is not finite)
    p.jacobian = (1 + s.z) * (1 + s.w) - shear * shear;
    const float cutoff = clamp(3.14159265 / max(p.footprint, 1e-4), o.kPeak, o.kMax);
    const float whole = 0.5 * o.alpha * log(o.kMax / max(o.kPeak, 1e-6));  // the spectrum's whole, up to the cascades' end
    p.variance = 0.5 * o.alpha * log(o.kMax / cutoff) + max(0.003 + 0.00512 * o.wind - whole, 0.0);
    return p;
}

// The mean Fresnel reflectance of water (R0 = 0.02) over the slopes a view at cosine cosV sees (the header's fit).
float oceanMeanFresnel(float cosV, float variance)
{
    const float sigma = sqrt(max(0.5 * variance, 0.0));
    return 0.02 + 0.98 * pow(saturate(1.0 - cosV), 5.0 * exp(-2.69 * sigma)) / (1.0 + 22.7 * pow(sigma, 1.5));
}

// The built-in sea water (the header's numbers): extinction sigma_t and scattering sigma_s per metre, the phase
// function's g.
void oceanWater(out float3 sigmaT, out float3 sigmaS, out float g)
{
    sigmaS = float3(0.020, 0.028, 0.036);
    sigmaT = float3(0.34, 0.064, 0.018) + sigmaS;
    g = 0.75;
}

// The foam over the sample, [0, 1]: the clipmap's at x0 and the shore's (waterDepth: the water over the bed at the
// sample, m; a deep sea: any large number).
float oceanFoam(OceanShade o, OceanSurfacePoint p, float waterDepth)
{
    float f = 0;
    if (o.foam != UNX_NONE) f = saturate(foamAt(o.foam, o.foamParams, p.x0, p.footprint));
    if (o.shoreFoam > 0 && waterDepth < o.shoreDepth)
    {
        const float shallow = saturate(1.0 - waterDepth / max(o.shoreDepth, 1e-3));
        f = max(f, o.shoreFoam * shallow * saturate(1.5 - p.jacobian));
    }
    return f;
}
#endif
