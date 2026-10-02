// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1 ML=0,1 GIV=0,1
// Particle render pass, per particle of the latest tick's render ranges (ParticleLayerPass.hlsli RenderRange):
//   STEP=0: the particle at the frame time (render rules request 2: a particle of both ticks by cubic Hermite of the two
//           ends' positions and velocities, one born in the latest tick by p_n - v_n (1 - w) dt from age_n - (1 - w) dt >= 0,
//           one that died in it by p_(n-1) + v_(n-1) w dt while w dt < lifetime - age_(n-1)), its appearance at that age
//           (size, colour, opacity: pure functions of program, emitter and age), the projection, the pixel-footprint
//           prefilter and the record; then the tile counts of the record's square.
//   STEP=1: the tile entries (depth bits, record index) at tile start + atomic fill (the tile kernel sorts them).
// Sprites only for now (M0); other outputs write an undrawn record. Lighting (request 3, stage 2): a program with material
// 1 is lit (colour = albedo): the sun (S's air visibility, the atmosphere's transmittance), R's GI cache (isotropic: the
// mean irradiance of the six axes / pi) and the froxel list's local lights (S's direct visibility where they cast
// shadows), each with the program's phase function (Henyey-Greenstein, g = medium_phase), once at the particle centre;
// material 0 is emissive (colour = radiance, nit). Every particle then takes S's air between the camera and its centre:
// premultiplied colour = alpha (T_air L + inscatter) (request 4: the surface behind already carries the full path).
// Sprite looks (material 2 + i: look i; unx/fx/SpriteLooks.h, ParticleLayerPass.hlsli): the sprite is a quad - its
// facing (the view plane, the camera's position, the velocity, an axis), rotation (the program's rotation curve + the
// look's rate), aspect, stretch by speed and pivot give two world axes, projected at the centre to the record's screen
// axes; the flipbook's frame at the particle's age; the light as a look takes it - emissive, lit as a medium, or for
// the tile kernel's per-pixel normal the light's fluence F and first moment M at the centre (the same sun, local
// light volume and indirect light): a surface of normal n receives F / 4 + M . n / 2, so the record holds
// albedo F / 4 pi and M / lum(F) in the quad's frame. Every sprite's record carries its centre's travel on screen
// since the previous frame (the particle moved back by its velocity over the frame, under the previous view).
// The light is evaluated once per thread (setup: a sprite's and a ribbon point's are the same call) - two copies of it
// put the variant with the lists' lights and the world cache over the kernel size limit.
#include "Passes/FX/ParticleLayerPass.hlsli"
#include "Passes/FX/FxParticleAt.hlsli"
#include "Passes/FX/ParticleShadow.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiSource.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"

static uint s_curveKeys;
float4 fxLayerCurveKey(uint i)
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
#define NV_CURVE_KEY(i) fxLayerCurveKey(i)
#include "Passes/FX/Stream/shaders/VfxParticleMath.hlsli"

float curve1(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).y : 1.0f; }

float fxPhase(float cosTheta, float g)  // Henyey-Greenstein, normalised over the sphere; g = 0: 1 / 4 pi
{
    const float g2 = g * g;
    return (1.0f - g2) / (4.0f * SH_PI * pow(max(1.0f + g2 - 2.0f * g * cosTheta, 1e-6f), 1.5f));
}

// The light at a particle (offset: camera-relative; D: camera -> particle, unit): what a medium of phase asymmetry g
// scatters towards the camera per unit albedo, and the light's fluence (the integral of the incident radiance over
// directions) with its first moment (luminance x direction to the source) - what a look lit per pixel shades with.
struct FxLight
{
    float3 scattered, fluence, moment;
};
float fxLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
FxLight fxLight(LayerConstants c, float3 offset, float3 D, float g, float footprint, uint2 pixel, float linearZ)
{
    const float3 worldPos = g_cameraPosition + offset;
    float3 L = 0, fluence = 0, moment = 0;
    AtmosphereSrvs atm;
    atm.transmittance = c.transmittance;
    atm.multiScatter = c.multiScatter;
    atm.skyView = UNX_NONE;
    atm.aerial = c.airVolume;
    ShadowSrvs sh;
    sh.pageTable = c.shadowPageTable;
    sh.pool = c.shadowPool;
    sh.blocks = c.shadowBlocks;
    sh.searchBound = c.shadowSearchBound;
    sh.constants = c.shadowConstants;
    sh.lights = c.shadowLights;
    sh.pad0 = c.shadowSlotOfLight;
    sh.layers = c.shadowLayers;
    // sun: illuminance at the particle (the atmosphere's transmittance), S's visibility in the air
    float3 E = g_sunIlluminance * g_sunColor;
    if (c.transmittance != UNX_NONE) E = atmosphereSunIlluminance(atm, worldPos);
    float visibility = 1;
    if (c.shadowPageTable != UNX_NONE)
    {
        bool resident;
        const float v = shadowSunVisibilityInAir(sh, worldPos, footprint, resident);
        if (resident) visibility = v;
    }
    // (the shadow-casting sprites between the particle and the sun, its own puff's upper part included)
    visibility *= fxParticleShadow(fxLayerExtra().shadowParams, worldPos);
    const float3 l = normalize(g_sunDirection);
    // (the sun's light is kept apart until the end: the ML variant's volume fetch brings the cloud layer's shadow on it)
    float3 sunE = E * visibility;
    // indirect (GiSource.hlsli): the Lumen translucency volume's light through the phase function (band 0, and band 1 x
    // g), or R's GI cache, isotropic (the mean irradiance over the six axes / pi = fluence / 4 pi)
    // (GIV: the source's kind picks the kernel - both reads in one kernel pass the DXIL limit; ParticleLayer.cpp)
#if GIV
    if (giSourceIsVolume(c.giCache) && giSourceVolume(c.giCache) != UNX_NONE)
    {
        // (ltvInscatter's value; the SH's band 0 is the fluence / (4 pi 0.282095), band 1 the moment x 3 / (4 pi 0.488603))
        const LtvSh sh = ltvSample(ltvParams(giSourceVolume(c.giCache)), worldPos);
        L += ltvInscatterOf(sh, D, g);
        fluence += (4.0f * SH_PI * 0.282095f) * sh.ambient;
        moment += (4.0f * SH_PI / 3.0f * 0.488603f) * float3(sh.directional.z, sh.directional.x, sh.directional.y);
    }
#else
    if (giSourceIsCache(c.giCache))
    {
        ByteAddressBuffer cache = ResourceDescriptorHeap[c.giCache];
        const GiHeader h = giHeader(cache);
        float3 sum = 0;
        float n = 0;
        const float3 axes[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
        float3 difference = 0;
        // (a loop: one copy of the cache read in the kernel - six unrolled were a third of its size)
        [loop] for (uint a = 0; a < 6u; ++a)
        {
            float w;
            const float3 e = giCacheIrradianceAt(cache, h, worldPos, axes[a], 0, w);
            if (w > 0) { sum += e; n += 1; difference += axes[a] * fxLuminance(e); }
        }
        if (n > 0) L += sum / (n * SH_PI);
        // (irradiance on normal n = fluence / 4 + moment . n / 2: the six axes' mean and their differences)
        if (n > 0) fluence += sum * (4.0f / n);
        if (n == 6) moment += difference;
    }
#endif
    // ML = 1 (its own variant: both paths in one kernel pass the DXIL limit; ParticleLayer.cpp picks it when the volumes exist):
    // shading.mega_lights (render A; P[1].xy = the froxel grid's sampled local light, MegaLightsVolume.hlsl - as Unreal's
    // MegaLights lights translucency through its lit volume): the local lights' visible fluence F and luminance-weighted
    // direction moment M at the particle's froxel (trilinear), with the phase function's first two SH bands:
    // L = F (1 + 3 g (M . D) / lum(F)) / 4 pi. The loop over the list with S's shadow maps is then not run (S assigns no
    // local shadow maps under mega_lights).
#if ML
    if (c.froxelLights != UNX_NONE && P[1].x != UNX_NONE)
    {
        const FroxelGrid grid = froxelGrid(c.froxelLights);
        const float3 uvw = float3((float2(pixel) + 0.5) / (float2(grid.gridX, grid.gridY) * grid.tilePx), max(froxelSliceCoord(grid, linearZ), 0.5) / grid.slices);
        Texture3D<float4> fluenceVolume = ResourceDescriptorHeap[P[1].x];
        Texture3D<float4> momentVolume = ResourceDescriptorHeap[P[1].y];
        // (alpha: the cloud layer's sun transmittance at the froxel - MegaLightsVolume.hlsl; B5 cloud shadow on lit particles)
        const float4 volumeFluence = fluenceVolume.SampleLevel(g_linearClamp, uvw, 0);
        const float3 F = volumeFluence.rgb / g_exposure;
        const float3 M = momentVolume.SampleLevel(g_linearClamp, uvw, 0).rgb / g_exposure;
        const float lumF = dot(F, float3(0.2126, 0.7152, 0.0722));
        if (lumF > 0) L += F * (max(0.0f, 1.0f + 3.0f * g * dot(M, D) / lumF) / (4.0f * SH_PI));
        fluence += F;
        moment += M;
        sunE *= volumeFluence.a;
    }
#else
    // local lights of the froxel list at the particle (punctual exactly; area lights as their centre's point, exact
    // when the light is small against its distance)
    if (c.froxelLights != UNX_NONE)
    {
        FroxelSrvs froxels;
        froxels.lights = c.froxelLights;
        froxels.lightIndices = c.froxelLights;
        froxels.scattering = UNX_NONE;
        froxels.pad = 0;
        const uint2 range = froxelLightRange(froxels, pixel, linearZ);
        for (uint i = 0; i < range.y; ++i)
        {
            const uint index = froxelLight(froxels, range.x + i);
            const GpuLight light = loadLight(index);
            float3 toLight;
            // (a medium scatters a light by its volumetric scattering scale, as the light volume of the branch above
            // holds it - not by its diffuse scale, which shPunctualIlluminance carries: Scene.hlsli; VolumeSetup.hlsl)
            const float volumetric = lightVolumetricScale(light);
            float3 El = shPunctualIlluminance(light, (light.position - g_cameraPosition) - offset, toLight) * (volumetric / max(lightDiffuseScale(light), 1e-4));
            if (lightType(light) > LIGHT_SPOT)
            {
                const float3 p = (light.position - g_cameraPosition) - offset;
                const float d2 = max(dot(p, p), 1e-4f);
                toLight = p * rsqrt(d2);
                // radiance x the light's projected area seen from the particle (small-source limit): rect w x h and disk
                // pi r^2 times their emitting side's cosine, sphere pi r^2, tube (capsule) 2 r l + pi r^2 broadside
                const uint type = lightType(light);
                const float facing = max(0.0f, dot(light.forward, -toLight));
                const float area = type == LIGHT_RECT ? light.size.x * light.size.y * facing
                                 : type == LIGHT_DISK ? SH_PI * light.size.x * light.size.x * facing
                                 : type == LIGHT_SPHERE ? SH_PI * light.size.x * light.size.x
                                                        : 2.0f * light.size.y * light.size.x + SH_PI * light.size.y * light.size.y;
                El = lightMeanColor(light) * (light.intensity * volumetric * area * lightBarnDoorFar(light, -toLight) * shAreaWindow(light, p) / d2);
            }
            float v = 1;
            if (lightCastsShadow(light) && c.shadowPageTable != UNX_NONE && c.shadowLights != UNX_NONE) v = shadowVisibilityDirect(sh, index, worldPos, -D);
            L += El * (v * fxPhase(dot(toLight, D), g));
            fluence += El * v;
            moment += toLight * fxLuminance(El * v);
        }
    }
#endif
    L += sunE * fxPhase(dot(l, D), g);
    fluence += sunE;
    moment += l * fxLuminance(sunE);
    FxLight o;
    o.scattered = L;
    o.fluence = fluence;
    o.moment = moment;
    return o;
}
// A world axis 'a' (view space) at view-space point v (distance = -v.z) on screen: the travel of the point's pixel per
// unit of the axis (the projection's derivative; no shear).
float2 fxProjectAxis(float3 v, float distance, float3 a)
{
    return float2(0.5f * g_viewWidth * g_proj[0][0] * (a.x * distance + v.x * a.z), -0.5f * g_viewHeight * g_proj[1][1] * (a.y * distance + v.y * a.z)) /
           (distance * distance);
}
float3 curve3(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).yzw : float3(1, 1, 1); }

// A ribbon particle's point of this frame in the render pass's ribbon layout (ParticleSystem: per ribbon row the births
// [dying_birth, next_birth) of the latest tick, the ones that died in it first; FxRibbon then builds the strips over each
// range's valid window): camera-relative position at the frame time, width after the
// pixel-footprint prefilter across it (the strip's profile widened to h' = sqrt(h^2 + 1/4) px, h its half width, at the same
// integrated opacity: alpha h / h'), and its appearance before the air (the strip samples the air at each hit):
// radiance x exposure (lit at the point like a sprite, else emissive), opacity. A point not alive at the frame time
// is written invalid: born after it (the newest births, the range's tail) or already dead (the oldest dying ones, its head);
// the valid points are one window. A killed emitter or a refused material writes invalid points (nothing drawn).
// alive: the particle at the frame time (setup); radiance, size, alpha, age: its appearance there.
// program, look: the point's program and its look + 1 (0: none), for the segment's record. moment: a look lit per pixel -
// the light's first moment over its fluence (world), else 0; motion: the point's travel on screen since the previous frame.
void ribbonPoint(LayerConstants c, uint birth, uint row, bool alive, float3 pos, float age, float size, float3 radiance, float alpha, uint program, uint look,
                 float3 moment, float2 motion)
{
    if (c.ribbonRows == UNX_NONE) return;
    StructuredBuffer<uint2> rows = ResourceDescriptorHeap[c.ribbonRows];
    const uint2 place = rows[row];
    if (place.x == FX_NONE) return;
    const uint index = place.x + (birth - place.y);
    if (index >= c.ribbonCapacity) { fxLayerStatus(c, FX_LAYER_STATUS_RANGE); return; }
    RWStructuredBuffer<FxRibbonPoint> points = ResourceDescriptorHeap[c.ribbonPoints];
    RWStructuredBuffer<FxRibbonAppearance> appearance = ResourceDescriptorHeap[c.ribbonAppearance];
    FxRibbonPoint rp = (FxRibbonPoint)0;
    FxRibbonAppearance app = (FxRibbonAppearance)0;
    if (!alive)
    {
        points[index] = rp;  // valid = 0
        appearance[index] = app;
        return;
    }
    const float distance = -mul((float3x3)g_view, pos).z;
    float width = max(size, 0.0f);
    if (distance > g_nearPlane && width > 0)
    {
        const float h = 0.5f * width * g_proj[1][1] * 0.5f * g_viewHeight / distance;
        const float hEff = sqrt(h * h + 0.25f);
        width *= hEff / h;
        alpha *= h / hEff;
    }
    rp.position = pos * c.streamAxes;  // (stream axes: FxRibbon builds the strip frame there and maps its vertices)
    rp.width = width;
    rp.age = age;
    rp.valid = 1u;
    rp.program = program;
    rp.look = look;
    points[index] = rp;
    app.radianceAlpha = fxPackHalf4(float4(radiance * g_exposure, alpha));
    app.moment = fxPackHalf4(float4(moment, 0));
    app.motion = fxPackHalf2(motion);
    appearance[index] = app;
}

LayerRecord setup(LayerConstants c, uint t, uint group)
{
    LayerRecord rec = (LayerRecord)0;
    const RenderRange rr = renderRange(c, t, group);
    const uint k = t - rr.thread, birth = rr.first + k, row = rr.row;
    StructuredBuffer<StreamEmitter> emitters = ResourceDescriptorHeap[c.emitters];
    StructuredBuffer<StreamProgram> programs = ResourceDescriptorHeap[c.programs];
    const StreamEmitter e = emitters[row];
    const StreamProgram p = programs[e.program];
    // The particle material contract (0 emissive: colour = nit; 1 lit: colour = albedo; 2 + i: look i of the renderer's
    // table, unx/fx/SpriteLooks.h): a value without a set look is refused, not guessed (an old material-table index drew
    // as emissive at the wrong scale).
    const LayerExtra x = fxLayerExtra();
    const bool sprite = p.output == FX_OUTPUT_SPRITE, ribbon = p.output == FX_OUTPUT_RIBBON;
    const bool looked = p.material > 1u;
    FxSpriteLook look = (FxSpriteLook)0;
    if (looked && x.looks != UNX_NONE && p.material - 2u < x.lookCount)
    {
        StructuredBuffer<FxSpriteLook> looks = ResourceDescriptorHeap[x.looks];
        look = looks[p.material - 2u];
    }
    const bool badMaterial = (sprite || ribbon) && looked && (look.flags & FX_LOOK_VALID) == 0u;
    if (WaveActiveAnyTrue(badMaterial) && WaveIsFirstLane()) fxLayerStatus(c, FX_LAYER_STATUS_MATERIAL);
    if (!sprite && !ribbon) return rec;
    const bool lit = looked ? (look.flags & FX_LOOK_LIT) != 0u : p.material == 1u;

    // the particle at the frame time and its appearance at that age
    float3 pos = 0, vel = 0;
    float age = 0;
    bool dying = false;
    bool alive = !badMaterial && (e.flags & FX_EMITTER_KILLED) == 0u && p.lifetime > 0 && fxParticleAtV(c, rr, k, birth, row, p, pos, vel, age, dying);
    const float u = alive ? saturate(age / p.lifetime) : 0.0f;
    const float size = p.size * e.sizeScale * curve1(p.sizeKeys, p.sizeCount, u);
    const float3 colour = p.color.rgb * e.colorScale.rgb * curve3(p.colorKeys, p.colorCount, u);
    float alpha = saturate(p.color.a * e.colorScale.a * curve1(p.alphaKeys, p.alphaCount, u));
    if (sprite && (!(size > 0) || !(alpha > 0))) alive = false;
    if (!alive)
    {
        if (ribbon) ribbonPoint(c, birth, row, false, 0, 0, 0, 0, 0, 0u, 0u, 0, 0);
        return rec;
    }

    // a look's quad: its world axes (right, up, facing: the normal toward the viewer), half sizes and centre
    float3 right = g_view[0].xyz, up = g_view[1].xyz, facing = g_view[2].xyz, centreWorld = pos;
    float halfU = 0.5f * size, halfV = 0.5f * size;
    if (looked && sprite)
    {
        const uint mode = fxLookFacing(look);
        const float3 toCamera = normalize(-pos);
        if (mode == FX_FACING_CAMERA_POSITION)
        {
            const float3 side = cross(g_view[1].xyz, toCamera);
            if (dot(side, side) > 1e-12f)
            {
                facing = toCamera;
                right = normalize(side);
                up = cross(facing, right);
            }
        }
        else if (mode == FX_FACING_VELOCITY || mode == FX_FACING_AXIS)
        {
            // the up axis along the velocity (or the look's axis), the quad turned about it toward the camera
            const float speed = length(vel);
            const float3 along = mode == FX_FACING_AXIS ? look.axis : (speed > 1e-6f ? vel / speed : g_view[1].xyz);
            const float3 side = cross(along, toCamera);
            if (dot(side, side) > 1e-12f)
            {
                up = along;
                right = normalize(side);
                facing = cross(right, up);
            }
            if (mode == FX_FACING_VELOCITY && look.stretch > 0)
            {
                const float factor = 1.0f + look.stretch * speed / max(size, 1e-6f);
                halfV *= look.stretchMax > 0 ? min(factor, look.stretchMax) : factor;
            }
        }
        if (mode <= FX_FACING_CAMERA_POSITION)
        {
            // rotation about the facing direction: the program's curve (radians) at this age + the look's rate
            const float angle = (p.rotationCount >= 2u ? nv_curve(p.rotationKeys, p.rotationCount, u).y : 0.0f) + look.rotationRate * age;
            float sn, cs;
            sincos(angle, sn, cs);
            const float3 r0 = right, u0 = up;
            right = r0 * cs + u0 * sn;
            up = u0 * cs - r0 * sn;
        }
        halfU *= look.aspect;
        centreWorld = pos - right * (look.pivot.x * halfU) - up * (look.pivot.y * halfV);
    }

    // projection (camera-relative: the view's rotation, then its projection)
    const float3 v = mul((float3x3)g_view, centreWorld);
    const float distance = -v.z;
    if (sprite && !(distance > g_nearPlane)) return rec;
    float2 centre = float2(0.5f * g_viewWidth, 0.5f * g_viewHeight);  // (a ribbon point at or behind the near plane: lit there)
    if (distance > g_nearPlane)
    {
        const float4 clip = mul(g_proj, float4(v, 1));
        const float2 ndc = clip.xy / clip.w;
        centre = float2((ndc.x + 1) * 0.5f * g_viewWidth, (1 - ndc.y) * 0.5f * g_viewHeight);
    }

    // the light, once (lit: colour = albedo; else emissive): at the centre, with the program's phase function for a
    // medium, as fluence and moment for a look lit per pixel
    const float linearZ = max(distance, g_nearPlane);  // (v.z along the view axis: the view depth)
    const float footprint = 2.0f * linearZ / (g_proj[1][1] * g_viewHeight);
    const bool pixelLit = lit && looked && fxLookNormal(look) != FX_NORMAL_NONE;
    float3 radiance = colour, moment = 0;
    if (lit)
    {
        const FxLight light = fxLight(c, centreWorld, normalize(centreWorld), p.mediumPhase, max(size * 0.5f, footprint),
                                      (uint2)clamp(centre, 0, float2(g_viewWidth - 1, g_viewHeight - 1)), linearZ);
        radiance = colour * light.scattered;
        if (pixelLit)
        {
            // (FxLayerTile.hlsl: albedo F / 4 pi, and the moment over the fluence's luminance in the quad's frame)
            radiance = colour * light.fluence / (4.0f * SH_PI);
            const float lum = fxLuminance(light.fluence);
            const float3 m = lum > 0 ? light.moment / lum : float3(0, 0, 0);
            // (a sprite's: in its quad's frame; a ribbon point's: world - the strip's frame is the pixel's, FxLayerTile)
            moment = sprite ? float3(dot(m, right), dot(m, up), dot(m, facing)) : m;
        }
    }
    // the centre's travel on screen since the previous frame: the particle moved back by its velocity over the frame,
    // under the previous (unjittered) view
    float2 motion = 0;
    if (any(x.prevViewProj[3] != 0) && distance > g_nearPlane)
    {
        const float4 before = float4(g_cameraPosition + centreWorld - vel * g_deltaTime, 1);
        const float3 pc = float3(dot(x.prevViewProj[0], before), dot(x.prevViewProj[1], before), dot(x.prevViewProj[3], before));
        if (pc.z > 1e-6f) motion = clamp(centre - float2((pc.x / pc.z * 0.5f + 0.5f) * g_viewWidth, (0.5f - 0.5f * pc.y / pc.z) * g_viewHeight), -32000.0f, 32000.0f);
    }
    if (ribbon)
    {
        ribbonPoint(c, birth, row, true, pos, age, size, radiance, alpha, e.program, looked ? p.material - 1u : 0u, moment, motion);
        return rec;
    }

    float rEff, alphaScale;
    bool small;
    if (looked)
    {
        float2 aU = fxProjectAxis(v, distance, mul((float3x3)g_view, right * halfU)), aV = fxProjectAxis(v, distance, mul((float3x3)g_view, up * halfV));
        const float lu = length(aU), lv = length(aV);
        if (!(lu > 0) || !(lv > 0)) return rec;
        // the pixel-footprint prefilter of the round sprites (below), along each axis
        const float eu = sqrt(lu * lu + 0.25f), ev = sqrt(lv * lv + 0.25f);
        aU *= eu / lu;
        aV *= ev / lv;
        alphaScale = lu * lv / (eu * ev);
        rEff = eu + ev;  // (the quad's corners lie within it)
        rec.axes = fxPackHalf4(float4(aU, aV));
        const uint frames = max(p.columns, 1u) * max(p.rows, 1u);
        float frame = min((float)p.firstFrame, frames - 1.0f);
        if (frames > 1u)
        {
            // the frames once over the particle's life (the last one at its death), or at the program's rate, looping
            if ((look.flags & FX_LOOK_FRAMES_OVER_LIFE) != 0u) frame += u * (frames - 1.0f - frame);
            else frame = fmod(frame + age * max(p.framesPerSecond, 0.0f), (float)frames);
        }
        rec.frame = frame;
        rec.uvOffset = fxPackHalf2(frac(p.uv.zw + p.uvScroll * age));
        // (a look's image has its own detail: the 1/4 layer takes only the sprites of a look marked smooth)
        small = (look.flags & FX_LOOK_SMOOTH) == 0u || min(eu, ev) < FX_LAYER_MIN_RADIUS;
    }
    else
    {
        const float radius = 0.5f * size * g_proj[1][1] * 0.5f * g_viewHeight / distance;
        // Pixel-footprint prefilter: the pixel box filter widens the profile to r' = sqrt(r^2 + 1/4) (radius of a half pixel)
        // at the same integrated opacity (alpha r^2 / r'^2): a sprite below a pixel keeps its energy instead of being missed
        // by pixel centres.
        const float r2 = radius * radius, rEff2 = r2 + 0.25f;
        rEff = sqrt(rEff2);
        alphaScale = r2 / rEff2;
        small = rEff < FX_LAYER_MIN_RADIUS;
    }
    if (centre.x + rEff < 0 || centre.y + rEff < 0 || centre.x - rEff > g_viewWidth || centre.y - rEff > g_viewHeight) return rec;
    rec.centre = centre;
    rec.radius = rEff;
    rec.depth = g_nearPlane / distance;
    // the air between the camera and the particle (S's air volume)
    float3 inscatter = 0;
    if (c.airVolume != UNX_NONE && c.transmittance != UNX_NONE)
    {
        AtmosphereSrvs atm;
        atm.transmittance = c.transmittance;
        atm.multiScatter = c.multiScatter;
        atm.skyView = UNX_NONE;
        atm.aerial = c.airVolume;
        float3 air, transmittance, sunAtDepth;
        atmosphereAirView(atm, centre / float2(g_viewWidth, g_viewHeight), linearZ, air, transmittance, sunAtDepth);
        // (a look's texture tints its own radiance, not the air in front: kept apart; an additive look hides nothing
        // and takes none of it)
        if (looked)
        {
            radiance *= transmittance;
            if (fxLookBlend(look) != FX_BLEND_ADDITIVE) inscatter = air;
        }
        else radiance = radiance * transmittance + air;
    }
    rec.radianceAlpha = fxPackHalf4(float4(radiance * g_exposure, alpha * alphaScale));
    rec.extra = fxPackExtra(moment, inscatter * g_exposure, motion);
    rec.flags = (small ? FX_LAYER_RECORD_SMALL : 0u) | (looked ? (p.material - 1u) << 8 : 0u);
    rec.program = e.program;
    return rec;
}

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    const LayerConstants c = fxLayerConstants();
    const uint t = id.x;
#if STEP == 0
    if (t >= c.threads) return;
#else
    if (t >= c.recordCapacity) return;
#endif
    RWStructuredBuffer<LayerRecord> records = ResourceDescriptorHeap[c.records];
#if STEP == 0
    s_curveKeys = c.curveKeys;
    const LayerRecord rec = setup(c, t, gid.x);
    records[t] = rec;
    uint2 t0, t1;
    if (!fxLayerTiles(c, rec, t0, t1)) return;
    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[c.tileCounts];
    [loop] for (uint y = t0.y; y <= t1.y; ++y)
        [loop] for (uint x = t0.x; x <= t1.x; ++x) InterlockedAdd(counts[y * c.tilesX + x], 1u);
    const uint n = WaveActiveCountBits(true);
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
    if (WaveIsFirstLane()) InterlockedAdd(counters[FX_LAYER_COUNTER_DRAWN], n);
#else
    // (the thread range covers the strip records too: FxLayerStrips at stripBase + point)
    const LayerRecord rec = records[t];
    uint2 t0, t1;
    if (!fxLayerTiles(c, rec, t0, t1)) return;
    RWStructuredBuffer<uint> fill = ResourceDescriptorHeap[c.tileFill];
    RWStructuredBuffer<uint> starts = ResourceDescriptorHeap[c.tileStarts];
    RWStructuredBuffer<uint2> entries = ResourceDescriptorHeap[c.entries];
    [loop] for (uint y = t0.y; y <= t1.y; ++y)
        [loop] for (uint x = t0.x; x <= t1.x; ++x)
        {
            const uint tile = y * c.tilesX + x;
            uint slot;
            InterlockedAdd(fill[tile], 1u, slot);
            const uint at = starts[tile] + slot;
            if (at < c.entryCapacity) entries[at] = uint2(asuint(rec.depth), t);
            else fxLayerStatus(c, FX_LAYER_STATUS_ENTRY_OVERFLOW);
        }
#endif
}
