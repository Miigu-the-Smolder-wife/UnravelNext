// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1,2
// The sun's particle transmittance map (ParticleShadow.hlsli; fx.particles.shadows), per frame:
//   STEP=0: one thread per texel clears it (optical depth 0, top 0, bottom at the far end); thread 0 writes the map's
//           parameters - its centre at the main camera, snapped to the texel grid along the two axes across the sun's
//           direction (a still sun: the same texels hold the same world columns from frame to frame).
//   STEP=1: one thread per render thread of the latest tick (the particle render pass's layout, FxParticleAt.hlsli): a
//           sprite of a look that casts a shadow, at the frame time, as a ball of its radius R - over the texels under
//           it, at distance d from its centre across the sun's direction (q = d^2 / R^2 < 1): the optical depth
//           -ln(1 - alpha (1 - q)^2) x the look's shadow density, added; the ball's chord there, centre +- R sqrt(1 - q)
//           along the sun's direction, widens the texel's span (atomic max and min). A ball wider than
//           FX_SHADOW_SPAN texels is taken over its middle FX_SHADOW_SPAN (the loop's bound). A look with a texture:
//           the opacity is alpha x the image's alpha at the texel (the frame of the particle's age, the level whose
//           texel covers a map texel), over the image's whole square.
//   STEP=2: S's screen visibility of a view, after S's passes (as the hair's shadow, HairShadow.hlsl MODE 0): the sun's
//           slot times the map's transmittance at the pixel's surface.
// STEP 0, 1: P[0] = { posAge cur, velocity cur, posAge prev, velocity prev }, P[1] = { dynamic cur, dynamic prev,
// emitters, programs }, P[2] = { curve keys, render ranges, render blocks, range count }, P[3] = { parameters UAV (raw),
// map UAV (raw), looks SRV, look count }, P[4] = { offsetCur.xyz, w }, P[5] = { offsetPrev.xyz, dt },
// P[6] = { streamAxes.xyz, render threads }, P[7] = { resolution, asuint(texel m), asuint(depth range m), map SRV };
// b1 = the main view (camera, sun). STEP 2: P[0] = { parameters SRV, the view's device depth, S's visibility UAV
// (R32_UINT), 0 }; b1 = the view.
#include "Passes/FX/ParticleShadow.hlsli"

#if STEP == 2
#include "Frame.hlsli"
#include "Scene.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].y];
    const float depth = depthTex[pixel];
    if (depth <= 0) return;
    RWTexture2D<uint> visibility = ResourceDescriptorHeap[P[0].z];
    const uint word = visibility[pixel];
    const uint sun = word & 0xFFu;
    if (sun == 0) return;
    const float through = fxParticleShadow(P[0].x, worldFromDepth(float2(pixel), depth));
    if (through < 1) visibility[pixel] = (word & 0xFFFFFF00u) | (uint)round(sun * through);
}
#else
#include "Passes/FX/ParticleLayerPass.hlsli"
#include "Passes/FX/FxParticleAt.hlsli"

#define FX_SHADOW_SPAN 96  // texels across a ball at most

static uint s_curveKeys;
float4 fxShadowCurveKey(uint i)
{
    StructuredBuffer<float4> keys = ResourceDescriptorHeap[s_curveKeys];
    return keys[i];
}
#define NV_PARTICLE_MATH_TYPES_ONLY
#include "Passes/FX/Stream/shaders/VfxParticleMath.hlsli"
#undef NV_PARTICLE_MATH_TYPES_ONLY
#define NV_FIELD_COUNT 0u
#define NV_FIELD(i) ((NvField)0)
#define NV_WORLD_FIELD_COUNT 0u
#define NV_WORLD_FIELD(i) ((NvWorldField)0)
#define NV_SURFACE_COUNT 0u
#define NV_SURFACE(i) ((NvSurface)0)
#define NV_CURVE_KEY(i) fxShadowCurveKey(i)
#include "Passes/FX/Stream/shaders/VfxParticleMath.hlsli"

float shadowCurve1(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).y : 1.0f; }

LayerConstants shadowConstants()
{
    LayerConstants c = (LayerConstants)0;
    c.posAgeCur = P[0].x, c.velocityCur = P[0].y, c.posAgePrev = P[0].z, c.velocityPrev = P[0].w;
    c.dynamicCur = P[1].x, c.dynamicPrev = P[1].y, c.emitters = P[1].z, c.programs = P[1].w;
    c.curveKeys = P[2].x, c.ranges = P[2].y, c.blocks = P[2].z, c.rangeCount = P[2].w;
    c.offsetCur = asfloat(P[4].xyz), c.w = asfloat(P[4].w);
    c.offsetPrev = asfloat(P[5].xyz), c.dt = asfloat(P[5].w);
    c.streamAxes = asfloat(P[6].xyz);
    return c;
}
// The map's parameters of this frame (STEP 0 writes them, STEP 1 uses the same values).
FxShadowParams shadowParams()
{
    FxShadowParams p;
    p.resolution = P[7].x;
    p.texel = asfloat(P[7].y);
    p.depthRange = asfloat(P[7].z);
    p.map = P[7].w;
    p.toSun = normalize(g_sunDirection);
    const float3 side = cross(abs(p.toSun.y) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0), p.toSun);
    p.axisU = normalize(side);
    p.axisV = cross(p.toSun, p.axisU);
    // the camera, snapped to the texel grid across the sun's direction
    const float2 across = round(float2(dot(g_cameraPosition, p.axisU), dot(g_cameraPosition, p.axisV)) / p.texel) * p.texel;
    p.centre = p.axisU * across.x + p.axisV * across.y + p.toSun * dot(g_cameraPosition, p.toSun);
    return p;
}

#if STEP == 0
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint n = P[7].x * P[7].x;
    RWByteAddressBuffer map = ResourceDescriptorHeap[P[3].y];
    if (id.x < n) map.Store3(FX_SHADOW_TEXEL_BYTES * id.x, uint3(0u, 0u, 0xFFFFFFFFu));
    if (id.x == 0)
    {
        RWByteAddressBuffer params = ResourceDescriptorHeap[P[3].x];
        params.Store<FxShadowParams>(0, shadowParams());
    }
}
#else
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    const uint t = id.x;
    if (t >= P[6].w || P[3].z == UNX_NONE) return;
    const LayerConstants c = shadowConstants();
    s_curveKeys = c.curveKeys;
    const RenderRange rr = renderRange(c, t, gid.x);
    const uint k = t - rr.thread, birth = rr.first + k, row = rr.row;
    StructuredBuffer<StreamEmitter> emitters = ResourceDescriptorHeap[c.emitters];
    StructuredBuffer<StreamProgram> programs = ResourceDescriptorHeap[c.programs];
    const StreamEmitter e = emitters[row];
    const StreamProgram p = programs[e.program];
    if (p.output != FX_OUTPUT_SPRITE || p.material < 2u || p.material - 2u >= P[3].w || (e.flags & FX_EMITTER_KILLED) != 0u || !(p.lifetime > 0)) return;
    StructuredBuffer<FxSpriteLook> looks = ResourceDescriptorHeap[P[3].z];
    const FxSpriteLook look = looks[p.material - 2u];
    if ((look.flags & (FX_LOOK_VALID | FX_LOOK_SHADOW)) != (FX_LOOK_VALID | FX_LOOK_SHADOW)) return;
    float3 pos;
    float age;
    bool dying;
    if (!fxParticleAt(c, rr, k, birth, row, p, pos, age, dying)) return;
    const float u = saturate(age / p.lifetime);
    const float R = 0.5f * p.size * e.sizeScale * shadowCurve1(p.sizeKeys, p.sizeCount, u);
    const float alpha = saturate(p.color.a * e.colorScale.a * shadowCurve1(p.alphaKeys, p.alphaCount, u));
    if (!(R > 0) || !(alpha > 0)) return;
    // a look's image: the frame of this age (FxLayerSetup.hlsl's rule, its floor)
    const bool textured = look.texture != UNX_NONE;
    const float2 cells = float2(max(p.columns, 1u), max(p.rows, 1u));
    const uint frames = max(p.columns, 1u) * max(p.rows, 1u);
    float frame = min((float)p.firstFrame, frames - 1.0f);
    if (frames > 1u)
    {
        if ((look.flags & FX_LOOK_FRAMES_OVER_LIFE) != 0u) frame += u * (frames - 1.0f - frame);
        else frame = fmod(frame + age * max(p.framesPerSecond, 0.0f), (float)frames);
    }

    const FxShadowParams m = shadowParams();
    const float3 d = g_cameraPosition + pos - m.centre;
    const float2 s = float2(dot(d, m.axisU), dot(d, m.axisV)) / m.texel + 0.5f * m.resolution;  // texel coordinates (a texel's centre at + 0.5)
    const float z = dot(d, m.toSun);
    const float reach = min(R / m.texel, 0.5f * FX_SHADOW_SPAN);
    const int2 lo = max((int2)floor(s - reach), 0), hi = min((int2)floor(s + reach), (int)m.resolution - 1);
    if (any(hi < lo)) return;
    RWByteAddressBuffer map = ResourceDescriptorHeap[P[3].y];
    const float2 perTexel = look.textureSize / cells * m.texel / (2.0f * R);  // the image's texels per map texel
    const float lod = max(log2(max(max(perTexel.x, perTexel.y), 1e-6f)), 0.0f);
    const float2 border = 0.5f * exp2(lod) * cells / max(look.textureSize, 1.0f);
    [loop] for (int y = lo.y; y <= hi.y; ++y)
        [loop] for (int x = lo.x; x <= hi.x; ++x)
        {
            const float2 o = (float2(x, y) + 0.5f - s) * m.texel;
            const float q = dot(o, o) / (R * R);
            float a;
            if (textured)
            {
                if (any(abs(o) > R)) continue;
                a = alpha * fxLookTexel(look.texture, (uint)frame, float2(0.5f + 0.5f * o.x / R, 0.5f - 0.5f * o.y / R), cells, border, lod).a;
            }
            else
            {
                if (q >= 1.0f) continue;
                a = alpha * (1.0f - q) * (1.0f - q);
            }
            const uint tau = (uint)round(-log(1.0f - min(a, 0.999f)) * look.shadowDensity / FX_SHADOW_TAU_UNIT);
            if (tau == 0u) continue;
            const float h = R * sqrt(saturate(1.0f - q));
            const uint top = (uint)round(saturate((z + h) / m.depthRange * 0.5f + 0.5f) * FX_SHADOW_DEPTH_STEPS);
            const uint bottom = (uint)round(saturate((z - h) / m.depthRange * 0.5f + 0.5f) * FX_SHADOW_DEPTH_STEPS);
            const uint at = FX_SHADOW_TEXEL_BYTES * ((uint)y * m.resolution + (uint)x);
            map.InterlockedAdd(at, tau);
            map.InterlockedMax(at + 4u, top);
            map.InterlockedMin(at + 8u, bottom);
        }
}
#endif
#endif
