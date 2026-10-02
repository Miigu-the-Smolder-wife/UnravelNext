// unx-kernel: cs_6_6 main
// s.fog.integrate (FogVolume.hlsli): one thread per column of the fog's volume, front to back. Per slice the light
// scattered inside it reaches the camera through what lies in front: with extinction s and source S (per metre) over a
// length d, the slice adds T x S (1 - e^(-s d)) / s and T becomes T e^(-s d) (the integral of a homogeneous slab: no
// energy is lost to the slice's own thickness). Stored per slice: rgb = radiance in-scattered between the camera and
// the slice's far face x the view's exposure (the scatter volume's sources are exposed: FogVolume.hlsli), a =
// transmittance to that face.
// Past the volume's end the integration goes on through zFar slices (fogFarDepth) with the exponential height fog in
// closed form (Fog.hlsli fogOpticalDepth) and the column's far source (nits per unit of optical depth): the sun through
// the phase function, outside the casters' shadow as the air volume found it for its own slices there (the fraction of
// each froxel slice's segment in shadow: FroxelIntegrate.hlsl, part 2's alpha - atmosphere.fog.far_shadows), and the
// indirect light at farM.
//
// What the volume holds is what a reader adds over the view's air (FogVolume.hlsli fogOverAir: in' = in_air x a + rgb,
// T' = T_air x a): the reader takes the air's light through all of the volume's media in front of the surface. Two
// things make that sum the picture of media that lie among each other (the column is integrated twice, for surfaces
// and for the sky):
//   order   atmosphere.fog.air_order - the air volume holds the atmosphere, the local lights' glow in it and the particle
//           media (smoke, fire) to every depth a surface is read at. A slab of this volume at depth t has that air in
//           front of it: its own light is taken through the air's transmittance to t, and the air's in-scattering up to
//           t, which the reader's "x a" removes by the slab's opacity, is put back by that opacity. With in_A and T_A the
//           air's in-scattering and transmittance to the slab (Atmosphere.hlsli airViewLookup at the slab's middle), the
//           slab adds T x (T_A x light + (1 - t) x in_A). Summed over the slabs this is the integral of the two media in
//           their order (the reader's sum less what it attenuates twice), so smoke in front of a fog bank is in front of
//           it, and a far fog bank is seen through the air before it. Only up to the farthest surface around the column
//           (the air volume is integrated no farther: its readers, P[4].x); past it the last values are held.
//   cloud   atmosphere.clouds.veil - the cloud layer in front of surfaces: the layer's own image (CloudMarch.hlsl) is of
//           whole view rays and only sky pixels take it. Here the column's ray is marched through the layer up to the
//           farthest surface around it, in P[4].w stretches (short at the span's start, growing by a constant ratio; each
//           stretch's density its mean: CloudCommon.hlsli cloudDensityFiltered) with the layer's light (CloudLight.hlsli:
//           the same sun path, octaves, sky and ground terms), and each stretch is a medium of the slices it crosses,
//           mixed with the fog there and ordered against the air as above. A ridge inside the layer is veiled by the
//           cloud in front of it and no more; the camera inside the layer sees the cloud from the first metres on (the
//           cells' slices are centimetres there).
// The sky's column is the fog alone (sky pixels show the layer's own image): it is stored in the last far slice, which
// only depths past the far end read (FogVolume.hlsli fogAt clamps there: fogOverSky). A surface in the last slice's span
// reads a blend toward it. With the order on, the sky's far slices are ordered too - against the air the far-field sky
// the pixel adds is made of: the air volume holds none along a sky ray, so it is integrated here beside the slices
// (fogSkyAirStep: the atmosphere's single scattering without casters and its multiple scattering, from the air's start
// on, two midpoint steps a slice). A ridge in the fog and the sky beside it are then ordered alike.
// P[0] = { grid x | y << 16, z | cell px << 16 | zFar << 24, asuint(far m), asuint(k) }
// P[1] = { asuint(b), scatter SRV (UNX_NONE: the cells hold no medium - the cloud alone), integrated UAV (Texture3D
//          RGBA16F, z + zFar slices), asuint(far end m) }
// P[2] = asuint{ density, height falloff, height, phase g }, P[3] = asuint{ albedo r, g, b, start distance }
// P[4] = { the air volume's readers SRV (FroxelTileDepth.hlsl: the farthest surface per froxel tile; UNX_NONE: every depth
//          is read), multi-scatter LUT SRV (the air lookup), bit 0: order, the cloud's stretches (0: no cloud here) }
// P[5] = { the cloud's lighting word (CloudSystem.cpp cloudSunWord: sun steps | filtered << 8 | ground light << 9), the
//          air volume SRV for the order and the cloud (UNX_NONE: neither takes the air), 0, 0 }
// P[6] = { air volume SRV (this frame's; UNX_NONE: the far fog without casters), froxel lights SRV (the air's grid;
//          UNX_NONE: neither far shadows nor readers), previous translucency volume params SRV (UNX_NONE: none),
//          transmittance LUT SRV }
// Frame constants of the view (the main view, or a planar reflection view).
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"
#include "Passes/Atmosphere/CloudLight.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/GI/LumenTranslucencyVolume.hlsli"

// The part of a stretch of the column's ray (view depths za < zb) outside the casters' shadow, from the air volume: slice
// s's shadowed fraction is at node s + 1 of part 2. Two taps in log depth, each between the two nodes around it and
// across the tiles (the parts are not blended into: whole nodes are fetched).
float fogFarLit(Texture3D<float4> air, FroxelGrid fg, float2 pixel, float za, float zb)
{
    uint w, h, d;
    air.GetDimensions(w, h, d);
    const float2 uv = pixel / (float2(w, h) * float(fg.tilePx));
    const float base = 2.0 * float(fg.slices + 1) + 0.5;
    float shadowed = 0;
    for (uint i = 0; i < 2; ++i)
    {
        const float depth = za * pow(zb / za, 0.25 + 0.5 * float(i));
        const float n = clamp(froxelSliceCoord(fg, depth) + 0.5, 1.0, float(fg.slices));
        const float n0 = floor(n), n1 = min(n0 + 1.0, float(fg.slices));
        shadowed += 0.5 * lerp(air.SampleLevel(g_linearClamp, float3(uv, (base + n0) / float(d)), 0).a,
                               air.SampleLevel(g_linearClamp, float3(uv, (base + n1) / float(d)), 0).a, n - n0);
    }
    return 1.0 - saturate(shadowed);
}

// The view's air in front of a depth of the column, as the readers take it (Atmosphere.hlsli airViewLookup): the
// in-scattering x the view's exposure, and the transmittance.
void fogAirAt(float2 uv, float depth, out float3 inScatter, out float3 transmittance)
{
    AtmosphereSrvs s;
    s.transmittance = P[6].w;
    s.multiScatter = P[4].y;
    s.skyView = 0xFFFFFFFFu;
    s.aerial = P[5].y;
    float3 sunT;
    airViewLookup(s, uv, depth, false, inScatter, transmittance, sunT);
    inScatter *= g_exposure;
    if (any(isnan(inScatter)) || any(isinf(inScatter)) || any(isnan(transmittance)))
    {
        inScatter = 0;
        transmittance = 1;
    }
}

// The cloud along the column's ray: its span in the layer [t0, t1] (m along the ray) in 'steps' stretches, stretch i
// from boundary i to i + 1, boundary i at (t0 + FOG_CLOUD_SHIFT) (ratio)^(i / steps) - FOG_CLOUD_SHIFT: a span that
// starts at the camera begins with stretches of a few metres, a distant one is cut nearly evenly. The walk holds the
// stretch the integration is in; a stretch is evaluated when the integration reaches it.
#define FOG_CLOUD_SHIFT 50.0
#define FOG_CLOUD_STEPS_MAX 128u
struct FogCloudWalk
{
    bool on;        // the integration has not passed the span's end
    float t0, t1;
    float steps, logRatio;
    int k;          // the stretch (-1: before the span)
    float end;      // where it ends (3e38: no cloud from here on)
    float rho;      // its mean density (1/m)
    float3 source;  // its light toward the camera per metre (nits x exposure / m)
};
void fogCloudWalkTo(inout FogCloudWalk w, CloudRecord c, CloudFrameLight light, float3 dir, uint sunWord, float at)
{
    [loop] while (w.on && w.end <= at)
    {
        ++w.k;
        w.rho = 0;
        w.source = 0;
        if (w.k >= (int)w.steps)
        {
            w.on = false;
            w.end = 3.0e38;
            break;
        }
        const float begin = w.end;
        w.end = w.k + 1 >= (int)w.steps ? w.t1 : (w.t0 + FOG_CLOUD_SHIFT) * exp2(w.logRatio * float(w.k + 1) / w.steps) - FOG_CLOUD_SHIFT;
        const float len = w.end - begin;
        if (!(len > 0)) continue;
        const float3 x = g_cameraPosition + dir * (begin + 0.5 * len);
        const bool filtered = (sunWord & 0x100u) != 0;
        const float rho = filtered ? cloudDensityFiltered(c, x, len) : cloudDensity(c, x);
        if (rho > 0)
        {
            w.rho = rho;
            w.source = cloudFrameSource(c, light, x, cloudFrameSunTau(c, x, sunWord & 0xFFu, filtered)) * (rho * g_exposure);
        }
    }
}

// The air along the column's ray over [t0, t1] (m along the ray) for the sky's column: the far-field sky's terms - no
// casters, no local lights -, advanced into the running in-scattering (x the view's exposure) and transmittance.
void fogSkyAirStep(AtmosphereParams ap, float3 dir, float t0, float t1, inout float3 inScatter, inout float3 transmittance)
{
    const float from = max(t0, AIR_VIEW_START_M), len = t1 - from;
    if (!(len > 0)) return;
    const float3 sun = normalize(g_sunDirection);
    const float nu = dot(dir, sun);
    const float3 x = airLiftToSurface(ap, g_cameraPosition + dir * (from + 0.5 * len));
    const AirCoefficients c = airCoefficients(ap, max(0.0, airAltitude(ap, x)));
    const float3 source = (c.rayleigh * airRayleighPhase(nu) + c.mie * airMiePhase(nu, ap.mieG)) * airSunTransmittance(ap, P[6].w, x, sun) +
                          (c.rayleigh + c.mie) * airMultipleScattering(ap, P[4].y, x, dir, sun);
    inScatter += transmittance * source * (g_sunIlluminance * g_sunColor * g_exposure) * airIntegral(c.extinction, len);
    transmittance *= exp(-c.extinction * len);
}

// One homogeneous stretch of the column: extinction sigma, light per metre 'source', length d, seen through T; the air in
// front of it (transmittance airT, in-scattering airIn: 1 and 0 where the stretch is not ordered against the air).
void fogStretch(float sigma, float3 source, float d, float3 airT, float3 airIn, inout float3 L, inout float T)
{
    const float t = exp(-sigma * d);
    const float3 light = sigma > 1e-7 ? source * ((1 - t) / sigma) : source * d;
    L += T * (airT * light + (1 - t) * airIn);
    T *= t;
}
// A slice of the column, [ta, tb] along its ray, with the fog as a uniform medium (sigma, source per metre) and the
// cloud's stretches that cross it. A stretch with cloud is ordered against the air whatever the fog's switch says.
void fogSliceWithCloud(inout FogCloudWalk w, CloudRecord c, CloudFrameLight light, float3 dir, uint sunWord, float ta, float tb, float sigma, float3 source,
                       bool order, float3 airT, float3 airIn, inout float3 L, inout float T)
{
    float at = ta;
    [loop] for (uint i = 0; i < FOG_CLOUD_STEPS_MAX + 2u && at < tb; ++i)
    {
        fogCloudWalkTo(w, c, light, dir, sunWord, at);
        const float end = min(tb, w.end);
        const bool withAir = order || w.rho > 0;
        fogStretch(sigma + w.rho, source + w.source, end - at, withAir ? airT : float3(1, 1, 1), withAir ? airIn : float3(0, 0, 0), L, T);
        at = end;
    }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const FogGrid g = fogGrid(P[0], P[1].x, P[1].w);
    if (id.x >= g.x || id.y >= g.y) return;
    RWTexture3D<float4> integrated = ResourceDescriptorHeap[P[1].z];
    const float2 pixel = (float2(id.xy) + 0.5) * float(g.cellPx);
    const float3 ray = froxelRayAt(pixel);
    const float toRay = length(ray);
    const float3 dir = ray / toRay;
    const float tStart = airViewStart(g_clipPlane, g_cameraPosition, dir);  // (a planar reflection view: from the mirror on)
    const float2 uv = pixel / float2(g_viewWidth, g_viewHeight);
    FroxelGrid fg = (FroxelGrid)0;
    if (P[6].y != 0xFFFFFFFFu) fg = froxelGrid(P[6].y);

    // What reads the column (view depths): the farthest surface in the froxel tiles whose air the column's lookups blend
    // (the two by two tiles around it: the air volume holds their air that far) and in the tiles around the column's own
    // (the surfaces that may read the column: the cloud is marched that far).
    const bool air = P[5].y != 0xFFFFFFFFu;
    // (the order is taken where there is a medium to order: the cells', then the far slices' height fog)
    const bool order = air && (P[4].z & 1u) != 0 && P[1].y != 0xFFFFFFFFu;
    float reachAir = 3.0e38, reachCloud = 3.0e38;
    if (P[4].x != 0xFFFFFFFFu && P[6].y != 0xFFFFFFFFu)
    {
        Texture2D<float2> readers = ResourceDescriptorHeap[P[4].x];
        const int2 lastTile = int2(fg.gridX, fg.gridY) - 1;
        const int2 tile = int2(pixel / float(fg.tilePx)), corner = int2(floor(pixel / float(fg.tilePx) - 0.5));
        reachAir = 0;
        reachCloud = 0;
        [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx) reachCloud = max(reachCloud, readers[clamp(tile + int2(dx, dy), 0, lastTile)].x);
        [unroll] for (int by = 0; by <= 1; ++by)
            [unroll] for (int bx = 0; bx <= 1; ++bx) reachAir = max(reachAir, readers[clamp(corner + int2(bx, by), 0, lastTile)].x);
    }

    // The cloud in front of the column's surfaces.
    FogCloudWalk walk = (FogCloudWalk)0;
    walk.end = 3.0e38;
    CloudRecord cloud = (CloudRecord)0;
    CloudFrameLight cloudLight = (CloudFrameLight)0;
    if (P[4].w != 0 && reachCloud > 0)
    {
        Texture2D<float4> lut = ResourceDescriptorHeap[P[6].w];
        uint lw, lh;
        lut.GetDimensions(lw, lh);
        const uint record = asuint(lut.Load(int3(10, lh - 1, 0)).z);  // (AtmosphereParams.clouds.x: the record's SRV + 1)
        if (record != 0)
        {
            cloud = cloudLoad(record - 1);
            const float volumeEnd = g.zFar != 0 ? g.farEndM : g.farM;
            float c0, c1;
            if (cloudShellSpan(cloud, g_cameraPosition, dir, min(reachCloud, volumeEnd) * toRay, c0, c1) && c1 > max(c0, tStart))
            {
                walk.on = true;
                walk.t0 = max(c0, tStart);
                walk.t1 = c1;
                walk.steps = float(min(P[4].w, FOG_CLOUD_STEPS_MAX));
                walk.logRatio = log2((walk.t1 + FOG_CLOUD_SHIFT) / (walk.t0 + FOG_CLOUD_SHIFT));
                walk.k = -1;
                walk.end = walk.t0;
                // (the light where the layer's own image takes it: at the middle of the ray's whole span)
                float whole0, whole1;
                cloudShellSpan(cloud, g_cameraPosition, dir, 3.0e38, whole0, whole1);
                cloudLight = cloudFrameLight(cloud, P[6].w, g_cameraPosition + dir * (0.5 * (whole0 + whole1)), dir, (P[5].x & 0x200u) != 0);
            }
        }
    }

    // L, T: what surfaces read; skyL, skyT: the fog alone, for the sky.
    float3 L = 0, skyL = 0;
    float T = 1, skyT = 1;
    float3 airT = 1, airIn = 0;
    float before = 0;
    [loop] for (uint z = 0; z < g.z; ++z)
    {
        float4 s = 0;
        if (P[1].y != 0xFFFFFFFFu)
        {
            Texture3D<float4> scatter = ResourceDescriptorHeap[P[1].y];
            s = scatter.Load(int4(id.xy, z, 0));
        }
        const float after = fogDepthOfSlice(g, float(z) + 1.0);
        const float d = (after - before) * toRay;
        const float sigma = max(s.a, 0.0);  // (-1: a cell that was not computed)
        const float3 source = s.a > 0 ? max(s.rgb, 0.0) : float3(0, 0, 0);
        const float t = exp(-sigma * d);
        skyL += skyT * (sigma > 1e-7 ? source * ((1 - t) / sigma) : source * d);
        skyT *= t;
        const bool cloudHere = walk.on && after * toRay > walk.t0;
        const float middle = 0.5 * (before + after);
        if (air && (order || cloudHere) && middle <= reachAir) fogAirAt(uv, middle, airIn, airT);
        if (cloudHere) fogSliceWithCloud(walk, cloud, cloudLight, dir, P[5].x, before * toRay, after * toRay, sigma, source, order, airT, airIn, L, T);
        else fogStretch(sigma, source, d, order ? airT : float3(1, 1, 1), order ? airIn : float3(0, 0, 0), L, T);
        before = after;
        // (without far slices the cells' last one is the sky's column)
        integrated[uint3(id.xy, z)] = g.zFar == 0 && z + 1 == g.z ? float4(min(skyL, 65504.0), skyT) : float4(min(L, 65504.0), T);
    }
    // the fog beyond the volume
    const FogMedium fog = fogMedium(uint4(1, 0, 0, 0), P[2], P[3]);
    const float3 p = g_cameraPosition + dir * max(g.farM * toRay, tStart);
    float3 fromSun = 0, fromAround = 0;
    const float3 E = g_sunIlluminance * g_sunColor;
    if (any(E > 0) && fog.density > 0)
    {
        const float3 sun = normalize(g_sunDirection);
        const AtmosphereParams a = airParamsFromTexels(P[6].w);
        fromSun = fog.albedo * E * airSunTransmittance(a, P[6].w, airLiftToSurface(a, p), sun) * airMiePhase(dot(dir, sun), fog.g);
    }
    if (P[6].z != 0xFFFFFFFFu && fog.density > 0) fromAround = fog.albedo * ltvInscatter(P[6].z, p, dir, fog.g);
    if (any(isnan(fromSun)) || any(isinf(fromSun))) fromSun = 0;
    if (any(isnan(fromAround)) || any(isinf(fromAround))) fromAround = 0;
    fromSun *= g_exposure;  // (exposed, as the cells' sources)
    fromAround *= g_exposure;
    const bool farShadows = P[6].x != 0xFFFFFFFFu && any(fromSun > 0);
    const bool orderFar = air && (P[4].z & 1u) != 0 && fog.density > 0;
    // (the sky's column: its air, integrated beside the slices)
    AtmosphereParams skyAir = (AtmosphereParams)0;
    if (orderFar) skyAir = airParamsFromTexels(P[6].w);
    float3 skyAirIn = 0, skyAirT = 1;
    [loop] for (uint i = 0; i < g.zFar; ++i)
    {
        const float za = fogFarDepth(g, float(i)), zb = fogFarDepth(g, float(i) + 1.0);
        const float tau = fogOpticalDepth(fog, g_cameraPosition, dir, max(za * toRay, tStart), zb * toRay);
        const float t = exp(-tau);
        // the sun in this slice: outside the casters' shadow, under the cloud layer at the slice's middle (in log depth)
        float lit = 1;
        if (any(fromSun > 0))
        {
            if (farShadows)
            {
                Texture3D<float4> airVolume = ResourceDescriptorHeap[P[6].x];
                lit = fogFarLit(airVolume, fg, pixel, za, zb);
            }
            lit *= cloudSunTransmittanceFromLut(P[6].w, g_cameraPosition + dir * max(sqrt(za * zb) * toRay, tStart));
        }
        const float3 farSource = fromSun * lit + fromAround;
        const float middle = sqrt(za * zb);
        if (orderFar)
        {
            fogSkyAirStep(skyAir, dir, max(za * toRay, tStart), middle * toRay, skyAirIn, skyAirT);
            skyL += skyT * (skyAirT * (farSource * (1 - t)) + (1 - t) * skyAirIn);
            fogSkyAirStep(skyAir, dir, max(middle * toRay, tStart), zb * toRay, skyAirIn, skyAirT);
        }
        else skyL += skyT * farSource * (1 - t);
        skyT *= t;
        const bool cloudHere = walk.on && zb * toRay > walk.t0;
        if (air && (orderFar || cloudHere) && middle <= reachAir) fogAirAt(uv, middle, airIn, airT);
        if (cloudHere)
        {
            // (the slice's fog as a uniform medium along it: its optical depth and its light per metre)
            const float perMetre = tau / max((zb - za) * toRay, 1e-6);
            fogSliceWithCloud(walk, cloud, cloudLight, dir, P[5].x, za * toRay, zb * toRay, perMetre, farSource * perMetre, orderFar, airT, airIn, L, T);
        }
        else
        {
            L += T * ((orderFar ? airT : float3(1, 1, 1)) * (farSource * (1 - t)) + (1 - t) * (orderFar ? airIn : float3(0, 0, 0)));
            T *= t;
        }
        // (the last slice: the sky's column)
        integrated[uint3(id.xy, g.z + i)] = i + 1 == g.zFar ? float4(min(skyL, 65504.0), skyT) : float4(min(L, 65504.0), T);
    }
}
