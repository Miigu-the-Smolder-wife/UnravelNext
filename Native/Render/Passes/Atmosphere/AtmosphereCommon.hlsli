// Physical atmosphere model shared by the S track's LUT kernels and its public lookups (Atmosphere.hlsli, Froxel.hlsli).
// Owner: S. Formulas and defaults are those of the previous engine (TitanNative Atmosphere*Program.h, Hillaire 2020 /
// Bruneton 2008): Rayleigh + Mie (Henyey-Greenstein) + ozone tent, spherical planet, the scene origin on its surface
// and the planet centre at (0, -bottomRadius, 0) (INTERFACES 8.3). Lengths in metres, coefficients in 1/m.
// LUT values are per unit solar illuminance at the top of the atmosphere; consumers multiply by E_TOA * sunColor.
#ifndef UNX_ATMOSPHERE_COMMON_HLSLI
#define UNX_ATMOSPHERE_COMMON_HLSLI
#include "Frame.hlsli"

#define ATMO_PI 3.14159265358979323846
#define ATMO_INV_4PI 0.07957747154594767

// Mirror of AtmosphereParams (AtmosphereSystem.h); 144 B (9 float4).
struct AtmosphereParams
{
    float bottomRadius, topRadius, rayleighScaleHeight, mieScaleHeight;
    float3 rayleighScattering;
    float mieG;
    float3 mieScattering;
    float ozoneCenter;
    float3 mieAbsorption;
    float ozoneWidth;
    float3 ozoneAbsorption;
    float froxelFarM;        // air volume (froxel grid, FroxelIntegrate.hlsl): depth of the last node
    float3 groundAlbedo;
    uint froxelSlices;
    uint2 transmittanceSize;
    uint2 multiScatterSize;
    uint2 skyViewSize;
    uint transmittanceSteps, multiScatterDirections;
    uint multiScatterSteps, skySegments, froxelTilePx;
    float froxelNearM;
};

AtmosphereParams airLoadParams(uint rawBuffer)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[rawBuffer];
    return b.Load<AtmosphereParams>(0);
}

// The same record from the 9 texels that MultiScatter.hlsl copies into the row below the multiple-scattering LUT.
AtmosphereParams airParamsFromTexels(uint multiScatterLut)
{
    Texture2D<float4> t = ResourceDescriptorHeap[multiScatterLut];
    uint w, h;
    t.GetDimensions(w, h);
    float4 q[9];
    [unroll] for (uint i = 0; i < 9; ++i) q[i] = t.Load(int3(i, h - 1, 0));
    AtmosphereParams a;
    a.bottomRadius = q[0].x; a.topRadius = q[0].y; a.rayleighScaleHeight = q[0].z; a.mieScaleHeight = q[0].w;
    a.rayleighScattering = q[1].xyz; a.mieG = q[1].w;
    a.mieScattering = q[2].xyz; a.ozoneCenter = q[2].w;
    a.mieAbsorption = q[3].xyz; a.ozoneWidth = q[3].w;
    a.ozoneAbsorption = q[4].xyz; a.froxelFarM = q[4].w;
    a.groundAlbedo = q[5].xyz; a.froxelSlices = asuint(q[5].w);
    a.transmittanceSize = asuint(q[6].xy); a.multiScatterSize = asuint(q[6].zw);
    a.skyViewSize = asuint(q[7].xy); a.transmittanceSteps = asuint(q[7].z); a.multiScatterDirections = asuint(q[7].w);
    a.multiScatterSteps = asuint(q[8].x); a.skySegments = asuint(q[8].y); a.froxelTilePx = asuint(q[8].z); a.froxelNearM = q[8].w;
    return a;
}

struct AirCoefficients
{
    float3 extinction, rayleigh, mie;
};

// Altitude of a point relative to the planet surface, stable for small heights (no difference of large numbers).
float airAltitude(AtmosphereParams a, float3 p)
{
    const float h2 = dot(p, p) + 2 * a.bottomRadius * p.y;
    return h2 / (sqrt(max(0.0, a.bottomRadius * a.bottomRadius + h2)) + a.bottomRadius);
}

float3 airUp(AtmosphereParams a, float3 p) { return normalize(p + float3(0, a.bottomRadius, 0)); }

// Scene geometry may lie below the model's surface (valleys under the origin's altitude). Air there is the air at the
// surface: points are lifted radially onto the surface for coefficients and sun transmittance. Used by the aerial and
// froxel integrals; the far-field sky rays start above the surface and never need it.
float3 airLiftToSurface(AtmosphereParams a, float3 p)
{
    const float h = airAltitude(a, p);
    return h >= 0 ? p : p + airUp(a, p) * (-h);
}

AirCoefficients airCoefficients(AtmosphereParams a, float h)
{
    AirCoefficients c = (AirCoefficients)0;
    if (h < 0 || h > a.topRadius - a.bottomRadius) return c;
    const float mieDensity = exp(-h / a.mieScaleHeight);
    c.rayleigh = a.rayleighScattering * exp(-h / a.rayleighScaleHeight);
    c.mie = a.mieScattering * mieDensity;
    c.extinction = c.rayleigh + c.mie + a.mieAbsorption * mieDensity + a.ozoneAbsorption * saturate(1 - abs(h - a.ozoneCenter) / a.ozoneWidth);
    return c;
}

// Ray (p + t d, unit d) against the sphere of 'radius' about the planet centre. Returns (near, far); near > far = miss.
// Computed relative to the surface origin so points near the ground keep precision.
float2 airSphere(AtmosphereParams a, float3 p, float3 d, float radius)
{
    const float b = dot(p, d) + a.bottomRadius * d.y;
    const float c = dot(p, p) + 2 * a.bottomRadius * p.y - (radius - a.bottomRadius) * (radius + a.bottomRadius);
    const float disc = b * b - c;
    if (disc < 0) return float2(1, -1);
    const float root = sqrt(disc);
    const float q = -b - (b < 0 ? -root : root);
    if (q == 0) return 0;
    return float2(min(q, c / q), max(q, c / q));
}

// Segment of the ray inside the atmosphere shell, before 'limit' and before the ground.
float2 airInterval(AtmosphereParams a, float3 p, float3 d, float limit)
{
    float2 shell = airSphere(a, p, d, a.topRadius);
    shell = float2(max(0.0, shell.x), min(limit, shell.y));
    const float2 ground = airSphere(a, p, d, a.bottomRadius);
    if (ground.y > shell.x && ground.x >= 0 && ground.x < shell.y) shell.y = ground.x;
    return shell;
}

// ---- Air volume of the main view (froxel grid): node n = 0..S at view depth z_0 = 0, z_n = near (far / near)^(n / S).
// Three parts of S + 1 depth slices each in one Texture3D (RGBA16F, gridX x gridY x 3 (S + 1)): part 0 in-scattering
// (rgb, pre-exposed), part 1 optical depth (rgb), part 2 sun transmittance at the node (rgb). Continuous node coordinate
// of a view depth, linear in z between nodes (the hardware interpolates between two slices with this weight).
float airNodeDepth(float nearM, float logRatio, float S, float n) { return n <= 0 ? 0.0 : nearM * exp2(logRatio * n / S); }
float airNodeCoord(float nearM, float farM, float S, float z)
{
    const float logRatio = log2(farM / nearM);
    const float z1 = airNodeDepth(nearM, logRatio, S, 1);
    if (z <= z1) return max(z, 0.0) / z1;
    const float n0 = min(floor(S * log2(z / nearM) / logRatio), S - 1);
    const float za = airNodeDepth(nearM, logRatio, S, n0), zb = airNodeDepth(nearM, logRatio, S, n0 + 1);
    return n0 + saturate((z - za) / (zb - za));
}

// A point on the surface (lifted, airLiftToSurface) may land a rounding error inside the sphere (near < 0 < far): it is
// blocked when the ray heads below its local horizon.
bool airHitsGround(AtmosphereParams a, float3 p, float3 d)
{
    const float2 ground = airSphere(a, p, d, a.bottomRadius);
    return ground.y > 0 && (ground.x >= 0 || dot(airUp(a, p), d) < 0);
}

float airRayleighPhase(float cosine) { return 0.05968310365946075 * (1 + cosine * cosine); }  // 3 / (16 pi)
float airMiePhase(float cosine, float g)
{
    const float den = 1 + g * g - 2 * g * cosine;
    return (1 - g * g) / (12.566370614359172 * den * sqrt(den));
}

// Integral of exp(-sigma t) over [0, distance], stable for small optical depth.
float3 airIntegral(float3 extinction, float distance)
{
    float3 result;
    [unroll] for (uint i = 0; i < 3; ++i)
    {
        const float tau = extinction[i] * distance;
        result[i] = tau < 1e-3 ? distance * (1 - tau * 0.5 + tau * tau / 6) : (1 - exp(-tau)) / extinction[i];
    }
    return result;
}

// 1 - exp(-x) without cancellation for small optical depths.
float3 airOneMinusExp(float3 x) { return select(x < 1e-3, x * (1 - x * (0.5 - x / 6)), 1 - exp(-x)); }

// ---- Main-view rays (frame constants of the main view bound).
// World direction through screen uv ([0,1]^2) of the view whose frame constants are bound.
float3 airViewDirection(float2 uv)
{
    const float4 p = mul(g_invViewProj, float4(uv.x * 2 - 1, 1 - uv.y * 2, 1, 1));
    return normalize(p.xyz / p.w - g_cameraPosition);
}
float3 airViewForward() { return -normalize(g_view[2].xyz); }

// ---- Transmittance LUT (Bruneton): u = distance to the top boundary between its min and max, v = rho / H.
// Rays in the LUT never hit the ground; callers test the ground separately.
float2 airTransmittanceUv(AtmosphereParams a, float altitude, float cosine)
{
    const float bottom = a.bottomRadius, top = a.topRadius, r = bottom + altitude;
    const float H = sqrt((top - bottom) * (top + bottom));
    const float rho = sqrt(max(0.0, altitude * (altitude + 2 * bottom)));
    const float d = -r * cosine + sqrt(max(0.0, r * r * cosine * cosine + (top - r) * (top + r)));
    const float dmin = top - r, dmax = rho + H;
    return saturate(float2((d - dmin) / max(dmax - dmin, 1e-3), rho / H));
}

void airTransmittanceParams(AtmosphereParams a, float2 uv, out float altitude, out float cosine)
{
    const float bottom = a.bottomRadius, top = a.topRadius;
    const float H = sqrt((top - bottom) * (top + bottom));
    const float rho = uv.y * H;
    altitude = rho * rho / (sqrt(rho * rho + bottom * bottom) + bottom);
    const float r = bottom + altitude;
    const float dmin = top - r, dmax = rho + H, d = lerp(dmin, dmax, uv.x);
    cosine = d <= 0 ? 1 : clamp((H * H - rho * rho - d * d) / (2 * r * d), -1.0, 1.0);
}

// Optical depth from (altitude, cosine) to the top: manual bilinear on texel centres of an RGBA32F LUT (hardware filter
// weights have 8 fractional bits, too coarse for optical depths of 10+ near the horizon).
float3 airOpticalDepth(AtmosphereParams a, uint lut, float altitude, float cosine)
{
    Texture2D<float4> t = ResourceDescriptorHeap[lut];
    const float2 p = airTransmittanceUv(a, max(0.0, altitude), cosine) * float2(a.transmittanceSize - 1);
    const uint2 lo = min(uint2(p), a.transmittanceSize - 1), hi = min(lo + 1, a.transmittanceSize - 1);
    const float2 f = p - float2(lo);
    const float3 v00 = t.Load(int3(lo, 0)).rgb, v10 = t.Load(int3(hi.x, lo.y, 0)).rgb;
    const float3 v01 = t.Load(int3(lo.x, hi.y, 0)).rgb, v11 = t.Load(int3(hi, 0)).rgb;
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

// Transmittance from p to space along d (zero when the ray hits the planet).
float3 airSunTransmittance(AtmosphereParams a, uint lut, float3 p, float3 d)
{
    if (airHitsGround(a, p, d)) return 0;
    return exp(-airOpticalDepth(a, lut, airAltitude(a, p), dot(airUp(a, p), d)));
}

// Multiple-scattering source Psi_ms (Hillaire 2020) per unit illuminance, indexed by sun cosine and altitude.
float3 airMultipleScattering(AtmosphereParams a, uint lut, float3 p, float3 sun)
{
    Texture2D<float4> t = ResourceDescriptorHeap[lut];
    const float altitude = saturate(airAltitude(a, p) / (a.topRadius - a.bottomRadius));
    const float2 q = float2(dot(airUp(a, p), sun) * 0.5 + 0.5, altitude) * float2(a.multiScatterSize - 1);
    const uint2 lo = min(uint2(q), a.multiScatterSize - 1), hi = min(lo + 1, a.multiScatterSize - 1);
    const float2 f = q - float2(lo);
    const float3 v00 = t.Load(int3(lo, 0)).rgb, v10 = t.Load(int3(hi.x, lo.y, 0)).rgb;
    const float3 v01 = t.Load(int3(lo.x, hi.y, 0)).rgb, v11 = t.Load(int3(hi, 0)).rgb;
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

// ---- Sky view (far field of the previous engine): elevation split at the true geometric horizon of the altitude
// (no interpolation across it), square-root spacing towards the horizon on each side; azimuth from the sun's
// vertical plane in [0, pi] (the field is symmetric about that plane). Rows [0, H/2) below the horizon, [H/2, H) above.
float airHorizonElevation(AtmosphereParams a, float altitude) { return -acos(saturate(a.bottomRadius / (a.bottomRadius + max(0.0, altitude)))); }

float airSkyElevation(AtmosphereParams a, float altitude, uint side, float u)
{
    const float horizon = airHorizonElevation(a, altitude);
    return side ? horizon + u * u * (1.5707963267948966 - horizon) : horizon - u * u * (horizon + 1.5707963267948966);
}

// Texel-space coordinate (x in [0, W-1], y within its half in [0, H/2-1]) and the half.
void airSkyCoordinates(AtmosphereParams a, float altitude, float3 up, float3 sun, float3 d, out float2 coord, out uint side)
{
    const float mu = clamp(dot(up, d), -1.0, 1.0), mus = dot(up, sun);
    const float elevation = asin(mu), horizon = airHorizonElevation(a, altitude);
    side = elevation >= horizon ? 1u : 0u;
    const float u = side ? sqrt(saturate((elevation - horizon) / (1.5707963267948966 - horizon)))
                         : sqrt(saturate((horizon - elevation) / (horizon + 1.5707963267948966)));
    const float3 dh = d - up * mu, sh = sun - up * mus;
    const float dl = length(dh), sl = length(sh);
    const float azimuth = (dl > 1e-7 && sl > 1e-7) ? acos(clamp(dot(dh, sh) / (dl * sl), -1.0, 1.0)) : 0;
    const uint half = a.skyViewSize.y / 2;
    coord = float2(azimuth / ATMO_PI * (a.skyViewSize.x - 1), u * (half - 1));
}

#endif
