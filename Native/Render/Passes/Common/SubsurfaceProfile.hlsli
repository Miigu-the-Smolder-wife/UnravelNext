// Subsurface class, stage B: the diffusion profile of the screen-space scattering pass. HLSL mirror of unx::scene::model
// (Native/Scene/include/unx/scene/MaterialModel.h "Subsurface class, stage B", which is authoritative and states the
// model); both must agree to float rounding (Passes/Test/SubsurfaceProfile.hlsl). Per colour channel:
//   R(r) = (e^(-r / d) + e^(-r / (3 d))) / (8 pi d r),   d = l / s(A),   s(A) = 1.9 - A + 3.5 (A - 0.8)^2
// l the material's mean free path (m), A the surface albedo; radial distribution p(r) = 2 pi r R(r), its integral P(r).
#ifndef UNX_SUBSURFACE_PROFILE_HLSLI
#define UNX_SUBSURFACE_PROFILE_HLSLI

#define SSS_MEAN_RADIUS 2.5  // of p, in units of d

float3 sssScaling(float3 albedo)
{
    const float3 a = saturate(albedo);
    return 1.9 - a + 3.5 * (a - 0.8) * (a - 0.8);
}
// d (m), at least 1e-6 (a channel without a mean free path keeps its light where it entered)
float3 sssDistance(float3 meanFreePath, float3 albedo) { return max(meanFreePath / sssScaling(albedo), 1e-6); }

float sssRadialPdf(float d, float r)
{
    const float y = exp(-r / (3 * d));
    return (y * y * y + y) / (4 * d);
}
float3 sssRadialPdf3(float3 d, float r)
{
    const float3 y = exp(-r / (3 * d));
    return (y * y * y + y) / (4 * d);
}
float sssRadialCdf(float d, float r)
{
    const float y = exp(-r / (3 * d));
    return 1 - 0.25 * (y * y * y) - 0.75 * y;
}
float3 sssRadialCdf3(float3 d, float r)
{
    const float3 y = exp(-r / (3 * d));
    return 1 - 0.25 * (y * y * y) - 0.75 * y;
}
float sssProfile(float d, float r) { return sssRadialPdf(d, r) / (2 * 3.14159265358979 * r); }

// P^-1(xi): y = e^(-r / (3 d)) is the real root of y^3 + 3 y = 4 (1 - xi) (Cardano, in the form that does not cancel)
float sssRadius(float d, float xi)
{
    const float u = max(1 - xi, 1e-6);
    const float c = pow(2 * u + sqrt(1 + 4 * u * u), 1.0 / 3.0), c2 = c * c;
    const float y = 4 * u / (c2 + 1 + 1 / c2);
    return -3 * d * log(min(y, 1.0));
}

// Sample pair k of 'pairs' beyond the pixel's own footprint (centreCdf = P(r_c) of the sampled channel): the radius at
// the stratum's point u (1 - u in odd strata: neighbours move against each other), the angle at the golden-ratio turn of k
// from u (u in [0, 1): the frame's blue noise).
float sssSampleRadius(float d, float centreCdf, uint k, uint pairs, float u)
{
    return sssRadius(d, centreCdf + (1 - centreCdf) * ((float(k) + ((k & 1u) != 0 ? 1 - u : u)) / float(pairs)));
}
float sssSampleAngle(uint k, float u) { return 6.28318530718 * frac(u + float(k) * 0.61803398875); }
// A sample's weight per channel: p_c at the distance sqrt(r^2 + h^2) from the pixel (r in the surface's plane, h off it)
// over the density pdf = p_s(r) its radius was drawn with.
float3 sssSampleWeight(float3 d, float r, float h, float pdf) { return sssRadialPdf3(d, sqrt(r * r + h * h)) / pdf; }

#endif
