// Material model v1 (INTERFACES_KO.md 8.1). HLSL mirror of unx::scene::model (Native/Scene/src/MaterialModel.cpp),
// which is authoritative; both must agree to float rounding. Owner: M.
#ifndef UNX_MATERIAL_MODEL_HLSLI
#define UNX_MATERIAL_MODEL_HLSLI
#include "Scene.hlsli"

#define MODEL_PI 3.14159265358979323846
#define MODEL_MIN_ALPHA 1e-4
#define MODEL_ALBEDO_TABLE_SIZE 32

struct ModelSurface
{
    uint cls;
    float3 baseColor;
    float roughness;
    float metallic;
    float specular;
    float transmission;
};

float modelAlpha(float roughness) { return max(roughness * roughness, MODEL_MIN_ALPHA); }
float3 modelF0(ModelSurface s) { return lerp((0.08 * s.specular).xxx, s.baseColor, s.metallic); }

// sinSqNH = |n x h|^2: the direct form avoids NoH^2 (a^2 - 1) + 1 cancelling to 0 at a = 1e-4 (mirror).
float modelD(float NoH, float sinSqNH, float alpha)
{
    const float a2 = alpha * alpha;
    const float t = sinSqNH + a2 * NoH * NoH;
    return a2 / (MODEL_PI * t * t);
}

float modelV(float NoV, float NoL, float alpha)
{
    const float a2 = alpha * alpha;
    const float gv = NoL * sqrt(NoV * NoV * (1 - a2) + a2);
    const float gl = NoV * sqrt(NoL * NoL * (1 - a2) + a2);
    return 0.5 / (gv + gl);
}

float3 modelFresnel(float3 f0, float VoH) { return f0 + (1 - f0) * pow(1 - saturate(VoH), 5.0); }

// Bilinear on the end-point-inclusive grid: identical addressing to directionalAlbedo() in C++.
float modelDirectionalAlbedo(float NoV, float roughness)
{
    StructuredBuffer<float> t = ResourceDescriptorHeap[g_materialModelLut];
    const float last = MODEL_ALBEDO_TABLE_SIZE - 1;
    const float x = saturate(NoV) * last, y = saturate(roughness) * last;
    const uint x0 = uint(x), y0 = uint(y);
    const uint x1 = min(x0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1), y1 = min(y0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1);
    const float fx = x - x0, fy = y - y0;
    const float a = t[y0 * MODEL_ALBEDO_TABLE_SIZE + x0], b = t[y0 * MODEL_ALBEDO_TABLE_SIZE + x1];
    const float c = t[y1 * MODEL_ALBEDO_TABLE_SIZE + x0], d = t[y1 * MODEL_ALBEDO_TABLE_SIZE + x1];
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

// Split specular directional albedo (A, B): f0 A + B = the lobe's albedo with Schlick Fresnel (specularAlbedo() in C++,
// same addressing as modelDirectionalAlbedo).
float2 modelSpecularAlbedo(float NoV, float roughness)
{
    StructuredBuffer<float2> t = ResourceDescriptorHeap[g_specularAlbedoLut];
    const float last = MODEL_ALBEDO_TABLE_SIZE - 1;
    const float x = saturate(NoV) * last, y = saturate(roughness) * last;
    const uint x0 = uint(x), y0 = uint(y);
    const uint x1 = min(x0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1), y1 = min(y0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1);
    const float fx = x - x0, fy = y - y0;
    const float2 a = t[y0 * MODEL_ALBEDO_TABLE_SIZE + x0], b = t[y0 * MODEL_ALBEDO_TABLE_SIZE + x1];
    const float2 c = t[y1 * MODEL_ALBEDO_TABLE_SIZE + x0], d = t[y1 * MODEL_ALBEDO_TABLE_SIZE + x1];
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

float3 modelEvaluate(ModelSurface s, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    const float3 albedo = s.baseColor * ((1 - s.metallic) / MODEL_PI);
    if (s.cls == MATERIAL_FOLIAGE && NoV * NoL < 0) return albedo * s.transmission;
    if (NoV <= 0 || NoL <= 0) return 0;
    const float3 h = normalize(v + l);
    const float NoH = saturate(dot(n, h)), VoH = saturate(dot(v, h));
    const float alpha = modelAlpha(s.roughness);
    const float3 f0 = modelF0(s);
    const float3 nxh = cross(n, h);
    const float3 single = modelFresnel(f0, VoH) * (modelD(NoH, dot(nxh, nxh), alpha) * modelV(NoV, NoL, alpha));
    const float e = modelDirectionalAlbedo(NoV, s.roughness);
    const float3 compensation = 1 + f0 * (1 / e - 1);
    const float3 diffuse = s.cls == MATERIAL_FOLIAGE ? albedo * (1 - s.transmission) : albedo;
    return diffuse + single * compensation;
}

#endif
