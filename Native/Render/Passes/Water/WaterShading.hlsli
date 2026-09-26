// Water dielectric shading functions (track W draft for M's Water material class; FEATURES_GAME 1.3, 1.5; render A
// registers the class in INTERFACES 8.1 and calls these). Exact where the quality table says exact:
//   Fresnel: the dielectric Fresnel equations for unpolarised light (mean of the s and p reflectances), not Schlick;
//            total internal reflection (R = 1) past the critical angle when leaving the denser medium.
//   Snell:   the refracted direction, or none under total internal reflection.
//   Energy:  a smooth interface splits the incident light into R + T = 1 exactly (T = 1 - R; no absorption at the
//            interface); the medium then attenuates the transmitted light by exp(-sigma_a d) per channel.
// Water: eta = 1.33 relative to air (dispersion ignored, quality table 1.5). Directions are unit vectors; `n` points
// out of the surface on the side of the incoming view direction `v` (v points away from the surface, towards the eye).
#ifndef UNX_WATER_SHADING_HLSLI
#define UNX_WATER_SHADING_HLSLI

static const float kWaterIor = 1.33;

// Unpolarised dielectric Fresnel reflectance for light meeting the interface at cos(theta_i) = cosI (> 0) from the
// medium of index n1 into n2 (eta = n1 / n2). Returns 1 under total internal reflection.
float waterFresnel(float cosI, float eta)
{
    cosI = saturate(cosI);
    float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T >= 1.0) return 1.0;
    float cosT = sqrt(1.0 - sin2T);
    // n1 cos_i vs n2 cos_t (divided by n2): rs = (eta cosI - cosT) / (eta cosI + cosT), rp = (eta cosT - cosI) / (eta cosT + cosI)
    float rs = (eta * cosI - cosT) / (eta * cosI + cosT);
    float rp = (eta * cosT - cosI) / (eta * cosT + cosI);
    return 0.5 * (rs * rs + rp * rp);
}

// Refraction of the view ray: the incident light travels along -v; returns false under total internal reflection.
// t is the transmitted direction (away from the surface, into the other medium).
bool waterRefract(float3 v, float3 n, float eta, out float3 t)
{
    float cosI = dot(v, n);
    float sin2T = eta * eta * max(0.0, 1.0 - cosI * cosI);
    t = float3(0, 0, 0);
    if (sin2T >= 1.0) return false;
    t = -eta * v + (eta * cosI - sqrt(1.0 - sin2T)) * n;
    return true;
}

// Mirror direction of v about n.
float3 waterReflect(float3 v, float3 n) { return 2.0 * dot(v, n) * n - v; }

// Transmittance of the water column along a path of length d (m) with absorption sigmaA (1/m per channel).
float3 waterAbsorption(float3 sigmaA, float d) { return exp(-sigmaA * max(d, 0.0)); }

// Radiance crossing the interface changes by (n_to / n_from)^2 (the solid angle compresses on entering the denser
// medium; flux is what R + T = 1 conserves). Seen from air through the surface, underwater radiance L_w arrives as
// T x L_w / 1.33^2; seen from under water, sky radiance arrives as T x L_air x 1.33^2.
float waterRadianceScale(bool fromAir) { return fromAir ? 1.0 / (kWaterIor * kWaterIor) : kWaterIor * kWaterIor; }

// Split of the light arriving at the interface: reflected fraction R and transmitted fraction T = 1 - R (energy
// conserving), for a view from air into water (fromAir) or from water into air.
float2 waterSplit(float cosI, bool fromAir)
{
    float R = waterFresnel(cosI, fromAir ? 1.0 / kWaterIor : kWaterIor);
    return float2(R, 1.0 - R);
}
#endif
