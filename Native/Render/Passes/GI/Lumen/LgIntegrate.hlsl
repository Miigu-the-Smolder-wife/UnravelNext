// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.integrate: the pixels' indirect light from the probes. Thread = pixel.
//   - The pixel's probes (LgInterpolate.hlsli) at its position moved by a per-pixel random offset of up to one tile
//     (P[2].x tiles; halved beyond 5-10 m; only while the moved pixel stays on the pixel's plane): the tile pattern
//     dissolves into noise the pixel filter removes. Weights under the minimum: the fallback weights. Normalised.
//   - One probe of the 4 is drawn with its weight (stochastic interpolation: P[2].y != 0), else all 4 are blended.
//   - Diffuse: the probe's irradiance map at the pixel's normal (bilinear over its 6 x 6 texels and border).
//   - Rough specular: 4 directions of the GGX lobe (roughness at least 0.2, visible normals) read from the probe's
//     radiance map with border, averaged in a compressed range (x / (1 + luminance)); from roughness 0.6 to 0.8
//     (P[2].z, fade 0.2) it goes over to irradiance / pi, which rougher surfaces take alone. The value is the lobe's mean
//     incident radiance (the reader applies its specular albedo).
// Output: diffuse RGBA16F = irradiance x exposure (as r.gi.screen's), a = the probes' moving share (at least 0.004;
// 0 = no probe: no lighting here); rough specular RGBA16F = radiance x exposure, a = 1 with data.
// P[0] = LgSurface inputs, P[1] = { diffuse UAV, rough specular UAV, irradiance map SRV (atlas x 8), radiance with border
// SRV (atlas x 10) }, P[2] = { jitter width in tiles (float), stochastic interpolation, max roughness for the lobe
// (float), probe moving SRV }, P[3] = { short-range AO SRV (A's ViewResources::shortRangeAO, RGBA16F: world bent normal
// x AO; 0xFFFFFFFF: none - lumen.short_range_ao off), max multibounce albedo (float), 0, 0 }: the irradiance is read
// along normalize(lerp(bent normal, normal, AO)) and multiplied by lumenAoMultibounce(base colour, AO), the rough
// specular lobe by lumenAoSpecular (LumenShortRangeAO.hlsli),
// P[10].z adaptive SRV, P[10].w / P[11].y probe depth / position SRVs. b1 = the view.
#include "Passes/GI/Lumen/LgInterpolate.hlsli"
#include "Passes/GI/LumenShortRangeAO.hlsli"

float3 lgIrradianceAt(Texture2D<float4> map, uint2 atlas, float3 n)
{
    const float2 uv = lgSphereInverse(n) * (float)LG_IRRADIANCE_RES + 1.0;
    uint w, h;
    map.GetDimensions(w, h);
    return map.SampleLevel(g_linearClamp, ((float2)(atlas * 8) + uv) / float2(w, h), 0).rgb;
}
float3 lgRadianceAt(Texture2D<float4> map, uint2 atlas, float3 direction)
{
    const float2 uv = lgSphereInverse(direction) * (float)LG_GATHER_RES + 1.0;
    uint w, h;
    map.GetDimensions(w, h);
    return map.SampleLevel(g_linearClamp, ((float2)(atlas * (LG_GATHER_RES + 2)) + uv) / float2(w, h), 0).rgb;
}
// A direction of the GGX lobe's visible normals (Heitz 2018), reflected: view v and normal n in world space.
float3 lgSampleLobe(float3 n, float3 v, float alpha, float2 u)
{
    const float3 up = abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 t = normalize(cross(up, n)), b = cross(n, t);
    const float3 ve = float3(dot(v, t), dot(v, b), max(dot(v, n), 1e-4));
    const float3 vh = normalize(float3(alpha * ve.x, alpha * ve.y, ve.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0 ? float3(-vh.y, vh.x, 0) / sqrt(lensq) : float3(1, 0, 0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u.x), phi = 2 * LG_PI * u.y;
    const float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    const float s = 0.5 * (1 + vh.z);
    p2 = (1 - s) * sqrt(saturate(1 - p1 * p1)) + s * p2;
    const float3 nh = p1 * t1 + p2 * t2 + sqrt(saturate(1 - p1 * p1 - p2 * p2)) * vh;
    const float3 h = normalize(float3(alpha * nh.x, alpha * nh.y, max(nh.z, 0.0)));
    const float3 l = reflect(-ve, h);
    return normalize(t * l.x + b * l.y + n * l.z);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= lgViewSize())) return;
    RWTexture2D<float4> diffuseOut = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<float4> specularOut = ResourceDescriptorHeap[P[1].y];
    const LgSurface s = lgSurface(id.xy);
    if (!s.valid)
    {
        diffuseOut[id.xy] = 0;
        specularOut[id.xy] = 0;
        return;
    }
    Texture2D<float4> irradiance = ResourceDescriptorHeap[P[1].z];
    Texture2D<float4> radiance = ResourceDescriptorHeap[P[1].w];
    Texture2D<float> probeMoving = ResourceDescriptorHeap[P[2].w];
    const float2 pixel = (float2)id.xy;
    float2 noiseOffset = 0;
    const float width = asfloat(P[2].x);
    if (width > 0)
    {
        const float effective = width * lerp(1.0, 0.5, saturate((s.depth - 5.0) / 5.0));
        const float2 offset = (lgNoise2(id.xy, lgFrame()) * 2 - 1) * (float)lgTile() * effective;
        const uint2 moved = (uint2)clamp(pixel + offset, 0.0, (float2)lgViewSize() - 1.0);
        const LgSurface sm = lgSurface(moved);
        if (sm.valid)
        {
            const float relative = abs(dot(float4(sm.position, -1), float4(s.normal, dot(s.position, s.normal)))) / s.depth;
            if (exp2(-1000000.0 * relative * relative) > 0.01) noiseOffset = offset;
        }
    }
    LgProbeSample ps;
    lgProbeWeights(pixel, noiseOffset, s.position, s.depth, s.normal, true, false, ps);
    bool lit = dot(ps.weights, 1) >= LG_MIN_INTERPOLATION_WEIGHT;
    if (!lit)
    {
        ps.weights = ps.fallback;
        lit = dot(ps.weights, 1) >= LG_MIN_INTERPOLATION_WEIGHT;
    }
    if (lit) ps.weights /= max(dot(ps.weights, 1), LG_MIN_INTERPOLATION_WEIGHT);
    else ps.weights = 0;
    if (P[2].y != 0)
    {
        const float u = min(lgNoise1(id.xy + 7919u, lgFrame()), 0.99) * dot(ps.weights, 1);
        uint2 picked = ps.atlas[0];
        if (u >= ps.weights[0] + ps.weights[1] + ps.weights[2]) picked = ps.atlas[3];
        else if (u >= ps.weights[0] + ps.weights[1]) picked = ps.atlas[2];
        else if (u >= ps.weights[0]) picked = ps.atlas[1];
        [unroll] for (uint c = 0; c < 4; ++c) ps.atlas[c] = picked;
        ps.weights = float4(lit ? 1.0 : 0.0, 0, 0, 0);
    }
    const float4 bent = lumenShortRangeAO(P[3].x, id.xy, s.normal);  // xyz = unit bent normal, w = AO ((n, 1) without the texture)
    const float ao = bent.w;
    const float3 lightingNormal = normalize(lerp(bent.xyz, s.normal, ao));
    const float3 aoDiffuse = P[3].x != 0xFFFFFFFFu ? lumenAoMultibounce(s.baseColor, ao, asfloat(P[3].y)) : float3(1, 1, 1);
    float3 D, Dx, Dy;
    mPixelRay(pixel + 0.5, D, Dx, Dy);
    const float3 v = -normalize(D);
    float3 e = 0, moving = 0;
    [unroll] for (uint c1 = 0; c1 < 4; ++c1)
    {
        if (!(ps.weights[c1] > 0)) continue;
        e += lgIrradianceAt(irradiance, ps.atlas[c1], lightingNormal) * ps.weights[c1];
        moving.x += probeMoving[ps.atlas[c1]] * ps.weights[c1];
    }
    e *= aoDiffuse;
    diffuseOut[id.xy] = float4(e, lit ? max(moving.x, 0.004) : 0.0);

    // rough specular
    float3 specular = e / LG_PI;
    const float fade = 0.2;
    const float diffuseLerp = saturate((s.roughness - asfloat(P[2].z) + fade) / fade);
    if (diffuseLerp < 1 && lit)
    {
        const float roughness = max(s.roughness, 0.2);
        const float alpha = roughness * roughness;
        float3 sum = 0;
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            float2 u = lgNoise2(id.xy + uint2(131u * (k + 1), 977u * (k + 1)), lgFrame());
            u.y = (u.y - 0.5) * 0.9 + 0.5;
            const float3 l = lgSampleLobe(s.normal, v, alpha, u);
            float3 value = 0;
            [unroll] for (uint c2 = 0; c2 < 4; ++c2)
                if (ps.weights[c2] > 0) value += lgRadianceAt(radiance, ps.atlas[c2], l) * ps.weights[c2];
            if (dot(l, s.normal) <= 0) value = 0;
            sum += value / (1 + dot(value, float3(0.2126, 0.7152, 0.0722)));
        }
        sum /= 4.0;
        const float aoSpecular = P[3].x != 0xFFFFFFFFu ? lumenAoSpecular(s.normal, roughness, ao, v, bent.xyz * ao) : 1.0;
        const float3 lobe = sum / max(1 - dot(sum, float3(0.2126, 0.7152, 0.0722)), 1e-3) * aoSpecular;
        specular = lerp(lobe, specular, diffuseLerp);
    }
    specularOut[id.xy] = float4(specular, lit ? 1.0 : 0.0);
}
