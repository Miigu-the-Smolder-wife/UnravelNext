// unx-kernel: cs_6_6 main
// m.ml.spatial (MegaLights.hlsli): the spatial step of the stochastic local lights and the result the shading kernels add.
// Per pixel, from m.ml.temporal's diffuse and specular (exposed, divided by the modulation factors), luminance moments,
// frame count n and history confidence:
//   disocclusion  d = 1 - (n - 1) / min(max disocclusion frames, frames the shading confidence allows - 1): the first
//                 frames after a history miss have no temporal variance yet, so the moments are taken over the 5 x 5
//                 neighbourhood (Gaussian), their standard deviation scaled, and the filter takes twice the samples;
//   filter        when the relative standard deviation says the pixel is still noisy (> 0.2 with d, else > 0.5 or the
//                 history confidence varies over the 5 x 5: the history was clamped in places), 'samples' taps on a disk
//                 of 'radius' pixels (Hammersley, a seed per 2 x 2 block and frame), each weighed by its distance from
//                 the pixel's tangent plane, its normal (diffuse: angle; specular: angle over the lobe's half angle)
//                 and its luminance difference over the standard deviation.
// Then the modulation factors come back and diffuse + specular is written (RGBA16F, exposed radiance; a = 1).
// P[0] = { diffuse (a = shading confidence), specular (a = valid), moments, frame counts (R8_UINT, n x 8) }
// P[1] = { history confidence (R8G8_UNORM), depth, G-buffer, material word }
// P[2] = { output UAV, width, height, flags (1: filter on, 2: use the history confidence's spatial deviation, 4: the
//          modulation factors are not multiplied back - the output takes the diffuse and P[5].x the specular: the coverage
//          layer's instance, whose fragments apply their own factors) }
// P[5] = { specular output UAV (flag 4), 0, 0, 0 }
// P[3] = { kernel radius (px, float), samples, depth weight scale (float), max disocclusion frames (float) }
// P[4] = { disocclusion deviation scale diffuse, specular, history confidence deviation threshold, max frames } (floats)
// ML_SPATIAL_SUBSURFACE = 1 (MegaLightsSpatialSubsurface.hlsl, m.ml.spatial.sss; shading.subsurface_scatter): the same
// filter on the Subsurface class's tiles, one group per tile of the class's list, for that class's pixels alone, with the
// two terms apart and the factors back: the output takes the diffuse per unit f_d (a = 0) - the first term of the class's
// diffuse texture (SubsurfaceScatter.hlsli) -, P[5].x the specular. Flag 4 is set (two outputs).
// P[6] = { tile list (raw), first entry, shade class, 0 }
#ifndef ML_SPATIAL_SUBSURFACE
#define ML_SPATIAL_SUBSURFACE 0
#endif
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/MegaLights.hlsli"

float2 mlDisk(float2 u)
{
    // concentric square -> disk (Shirley, Chiu)
    const float2 a = 2 * u - 1;
    if (a.x == 0 && a.y == 0) return 0;
    float r, phi;
    if (abs(a.x) > abs(a.y))
    {
        r = a.x;
        phi = (3.14159265 / 4) * (a.y / a.x);
    }
    else
    {
        r = a.y;
        phi = 3.14159265 / 2 - (3.14159265 / 4) * (a.x / a.y);
    }
    return r * float2(cos(phi), sin(phi));
}

[numthreads(8, 8, 1)]
#if ML_SPATIAL_SUBSURFACE
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    ByteAddressBuffer classTiles = ResourceDescriptorHeap[P[6].x];
    const uint classTile = classTiles.Load(4 * (P[6].y + gid.x));
    const uint2 pixel = uint2(classTile & 0xFFFFu, classTile >> 16) * M_TILE + tid;
    const uint2 size = P[2].yz;
    if (any(pixel >= size)) return;
    {
        // (pixels of other classes in the tile keep the diffuse texture's 0)
        Texture2D<uint> classWords = ResourceDescriptorHeap[P[1].w];
        const uint classMaterial = mWordMaterial(classWords[pixel]);
        if (classMaterial == M_MATERIAL_SKY || mShadeClassOf(loadMaterial(classMaterial)) != P[6].z) return;
    }
#else
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    const uint2 size = P[2].yz;
    if (any(pixel >= size)) return;
#endif
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[2].x];
    Texture2D<float4> diffuseTex = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> specularTex = ResourceDescriptorHeap[P[0].y];
    Texture2D<float4> momentsTex = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint> framesTex = ResourceDescriptorHeap[P[0].w];
    const float count = framesTex[pixel] / 8.0;
    if (!(count > 0))
    {
        output[pixel] = 0;
        if ((P[2].w & 4u) != 0)
        {
            RWTexture2D<float4> outputSpecular = ResourceDescriptorHeap[P[5].x];
            outputSpecular[pixel] = 0;
        }
        return;
    }
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[1].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[1].z];
    Texture2D<uint> words = ResourceDescriptorHeap[P[1].w];
    const float4 centreD = diffuseTex[pixel];
    const float3 centreS = specularTex[pixel].rgb;
    const uint word = words[pixel];
    const GpuMaterial m = loadMaterial(mWordMaterial(word));
    const GBufferSample g = decodeGBuffer(gbuffer[pixel]);
    const float linearZ = linearDepth(depthTex[pixel]);
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 worldPos = g_cameraPosition + D * linearZ;
    const float3 v = -normalize(D);
    const float3 n = mNormalTowardsViewer(g.normal, v);
    const float NoV = dot(n, v);

    float3 diffuse = centreD.rgb, specular = centreS;
    if ((P[2].w & 1u) != 0)
    {
        const float maxFrames = asfloat(P[4].w);
        const float confidenceFrames = mlConfidenceFrames(centreD.a, maxFrames);
        // (quantised as the frame counts are stored)
        const float maxDisocclusion = min(asfloat(P[3].w), floor(confidenceFrames * 8 + 0.5) / 8 - 1);
        const float disocclusion = maxDisocclusion > 0 ? 1 - saturate((count - 1) / maxDisocclusion) : 0;
        float4 moments = 0;
        int oy, ox;
        if (disocclusion > 0.01)
        {
            float weight = 0;
            for (oy = -2; oy <= 2; ++oy)
                for (ox = -2; ox <= 2; ++ox)
                {
                    const int2 c = clamp(int2(pixel) + int2(ox, oy), int2(0, 0), int2(size) - 1);
                    const float w = exp2(-2.0 * float(ox * ox + oy * oy) / 9.0);
                    moments += momentsTex[c] * w;
                    weight += w;
                }
            moments /= weight;
        }
        else moments = momentsTex[pixel];
        float sdD = sqrt(max(moments.y - moments.x * moments.x, 0.0)), sdS = sqrt(max(moments.w - moments.z * moments.z, 0.0));
        bool filterD = false, filterS = false;
        if (disocclusion > 0.01)
        {
            sdD *= asfloat(P[4].x);
            sdS *= asfloat(P[4].y);
            filterD = sdD / max(moments.x, 0.1) > 0.2;
            filterS = sdS / max(moments.z, 0.1) > 0.2 && confidenceFrames >= 4.0;
        }
        else
        {
            bool2 noisy = false;
            if ((P[2].w & 2u) != 0)
            {
                Texture2D<float2> confidenceTex = ResourceDescriptorHeap[P[1].x];
                float2 sum = 0, sq = 0;
                float weight = 0;
                for (oy = -2; oy <= 2; ++oy)
                    for (ox = -2; ox <= 2; ++ox)
                    {
                        if ((ox == 0 && oy == 0) || (abs(ox) == 2 && abs(oy) == 2)) continue;
                        const int2 c = int2(pixel) + int2(ox, oy);
                        const float2 h = all(c >= 0) && all(c < int2(size)) ? confidenceTex[c] : float2(1, 1);
                        sum += h;
                        sq += h * h;
                        weight += 1;
                    }
                const float2 mean = sum / weight;
                noisy = sqrt(max(sq / weight - mean * mean, 0.0)) > asfloat(P[4].z);
            }
            if ((confidenceFrames >= 8.0 || any(noisy)) && maxFrames > 1.0)
            {
                filterD = sdD / max(moments.x, 0.1) > 0.5 || noisy.x;
                filterS = sdS / max(moments.z, 0.1) > 0.5 || noisy.y;
            }
        }
        const float radius = asfloat(P[3].x);
        if (radius > 0 && (filterD || filterS))
        {
            const uint taps = uint(lerp(float(P[3].y), 2.0 * float(P[3].y), disocclusion) + 0.5);
            const uint seed = mlHash((pixel.x & ~1u) * 0x9E3779B1u ^ (pixel.y & ~1u) * 0x85EBCA6Bu ^ g_frameIndex * 0xC2B2AE35u);
            float halfAngle = atan(3.0 * g.roughness * g.roughness) + 0.01;  // the lobe keeping 3/4 of its energy
            halfAngle = clamp(halfAngle * lerp(1.0, 2.0, disocclusion), 0.01, 3.14159265 / 2);
            const float lumD = mlLuminance(centreD.rgb), lumS = mlLuminance(centreS);
            float3 sumD = centreD.rgb, sumS = centreS;
            float weightD = 1, weightS = 1;
            for (uint k = 0; k < taps; ++k)
            {
                const float2 u = float2(frac(float(k) / taps + (seed & 0xFFFFu) / 65536.0), ((reversebits(k) >> 16) ^ (seed >> 16)) / 65536.0);
                const int2 c = int2(floor(float2(pixel) + mlDisk(u) * radius + 0.5));
                if (any(c < 0) || any(c >= int2(size))) continue;
                const float4 nS = specularTex[c];
                if (!(nS.a > 0)) continue;
                float3 nDir, nDx, nDy;
                mPixelRay(float2(c) + 0.5, nDir, nDx, nDy);
                const float plane = dot(g_cameraPosition + nDir * linearDepth(depthTex[c]) - worldPos, n) / linearZ;
                const float depthWeight = exp2(-asfloat(P[3].z) * plane * plane);
                const float angle = acos(saturate(dot(n, octDecode(gbuffer[c].x))));
                if (filterD)
                {
                    const float3 nD = diffuseTex[c].rgb;
                    const float w = depthWeight * (1 - saturate(angle)) * exp2(-abs(lumD - mlLuminance(nD)) / max(sdD, 0.001));
                    sumD += nD * w;
                    weightD += w;
                }
                if (filterS)
                {
                    const float w = depthWeight * (1 - saturate(angle / halfAngle)) * exp2(-abs(lumS - mlLuminance(nS.rgb)) / max(sdS, 0.001));
                    sumS += nS.rgb * w;
                    weightS += w;
                }
            }
            diffuse = sumD / weightD;
            specular = sumS / weightS;
        }
    }
    const float metallic = mWordMetallic(word);
#if ML_SPATIAL_SUBSURFACE
    {
        // diffuse x its factor is the lights' diffuse radiance f_d E x exposure: E x exposure, within f16 (f_d under 1e-5: 0 with it)
        RWTexture2D<float4> outputSpecular = ResourceDescriptorHeap[P[5].x];
        const float3 fd = g.baseColor * ((1 - metallic) / 3.14159265);
        output[pixel] = float4(min(diffuse * mlDiffuseFactor(g.baseColor, metallic) / max(fd, 1e-5), 60000.0), 0);
        outputSpecular[pixel] = float4(specular * mlSpecularFactor(g.baseColor, metallic, m.specular, g.roughness, NoV), 1);
        return;
    }
#endif
    if ((P[2].w & 4u) != 0)
    {
        RWTexture2D<float4> outputSpecular = ResourceDescriptorHeap[P[5].x];
        output[pixel] = float4(diffuse, 1);
        outputSpecular[pixel] = float4(specular, 1);
        return;
    }
    const float3 lighting = diffuse * mlDiffuseFactor(g.baseColor, metallic) + specular * mlSpecularFactor(g.baseColor, metallic, m.specular, g.roughness, NoV);
    output[pixel] = float4(lighting, 1);
}
