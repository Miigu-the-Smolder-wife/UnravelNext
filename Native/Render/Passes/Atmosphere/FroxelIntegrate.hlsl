// unx-kernel: cs_6_6 main
// Air volume of the main view on the froxel grid (ARCHITECTURE 2.3 "볼륨 산란"; Atmosphere.hlsli atmosphereAerial /
// atmosphereAirView). One group per screen tile, one thread per depth slice; slice s is the segment of the tile-centre
// ray between nodes s and s + 1 (node 0 = camera, node S = far_m). Per slice, midpoint substeps of at most
// air_step_altitude_m altitude change (exact exponential within each):
//  - the atmosphere's single scattering with the tile-centre phase, times (1 - f), f the segment's fraction shadowed by
//    casters (vsmAirShadowFraction, at shadow_texels_per_tile texels per tile width); multiple scattering (Hillaire's
//    Psi_ms; its sources are the whole sky and are not reduced by the casters' shadows);
//  - local lights of the froxel's list: in-scattering by the air, int e^(-sigma_t t) (sigma_R P_R + sigma_M P_M) I(w)
//    window(d) / d^2 dt, integrated in the angle subtended at the light (t = t_c + h tan theta, dt / d^2 = dtheta / h:
//    the integrand is smooth in theta) with 8-point Gauss-Legendre. Area lights act as point sources of their
//    projected intensity (froxelIntensity). A light with a shadow slot (list entry bit 15) shadows its air: its
//    integral takes AIR_SHADOW_POINTS uniform midpoints in the angle instead (the visibility is a step function there,
//    which Gauss-Legendre points bias), each tested against the light's VSM at the mip whose texel matches the froxel's
//    lateral resolution (lit where no page holds it; VsmLocalMarkAir requests the same points' pages);
//  - optical depth.
// A group scan gives node n = sum over slices j < n of e^(-tau before j) x source_j. Volume (RGBA16F, gridX x gridY x
// 3 (S + 1)): part 0 the in-scattering normalised by the node's 1 - T, L / (1 - T), x exposure of the view (fp16 keeps
// the relative precision of what is displayed, also at night exposures where local lights' air glow is 1e-4 nits),
// part 1 optical depth, part 2 sun transmittance at the node. L / (1 - T) is the transmittance-weighted mean source of
// the path: it varies slowly across tiles where L itself peaks between two tile rows (rays grazing the horizon, one of
// them lifted below the model's surface), so the lookup blends it across tiles and multiplies by the pixel's own
// 1 - T (optical depth blended in altitude across that kink): 30 km horizon query at 1080p 3.1 % -> 0.9 % against
// the reference (FroxelTests 4). Nodes without air yet (node 0; planar views before the mirror) take the first node
// with air (the limit of the ratio). No local media in scenes v1 (their optical depth adds to part 1).
// Only the slices a reader reaches are integrated (P[3].z, FroxelTileDepth.hlsl): surface pixels of the tile's 3 x 3
// neighbourhood (the lookups blend across tiles) read nodes up to their depth; sky pixels there read the sky correction,
// which is nonzero only in slices where a caster can shadow the air (some point below the casters' light-space top hMax)
// or a local light is listed, and needs the optical depth before them. The other slices hold nothing (their nodes are
// never read): upward and far slices of sky and near-wall tiles took the most substeps. Exact: the nodes a reader
// reaches are the same numbers as with every slice integrated (FroxelTests 6).
// Slice 3 (S + 1), one per tile: the sky correction, pre-exposed and signed: along the tile ray to far_m, the local
// lights' in-scattering minus the single scattering the casters' shadows remove, each attenuated to the camera. Sky
// pixels add it to the far-field sky (atmosphereSkyRadianceView): the sky LUT has neither.
// Planar reflection views (clip plane in the view's frame constants, v1.22): each tile ray's air starts where it crosses
// the mirror (airViewStart); the slices before it hold nothing (the main view's mirror pixel applies that air), the sun
// transmittance of their nodes is taken at the nodes' mirror images (the real path).
// P[0].x froxelLights SRV (raw), P[0].y volume UAV (RWTexture3D<float4>), P[0].z transmittance LUT, P[0].w multi-scatter LUT
// P[1].x VSM page table SRV (raw), .y pool SRV (raw), .z blocks SRV (raw), .w VSM constants CBV (0xFFFFFFFF: no VSM)
// P[3].x local lights SRV (StructuredBuffer<VsmLocalLight>; 0xFFFFFFFF: none), P[3].y slot of light SRV, P[3].z tile
// readers SRV (Texture2D<float2>, FroxelTileDepth.hlsl; 0xFFFFFFFF: every slice, tests), P[3].w VSM stats UAV (raw; with
// the VSM: the error word VSM_STATS_ERROR_BYTE, and with P[2].w bit 16 the walk statistics, words 20..24,
// atmosphere.froxels.walk_stats, measurement only)
// P[2].x VSM search bound SRV (raw), P[2].y shadow texels per tile (float bits), P[2].z air step altitude m (float bits),
// P[2].w experiment mask (atmosphere.froxels.experiment_disable; 0; cost attribution only: 1 air shadows, 2 local lights,
// 4 air integration, 8 sun transmittance per substep, 16 multiple scattering per substep, 32 air shadow walk stops at
// the page level; bit 16: walk statistics on)
// Light functions (A8, E's Passes/Lights/LightFunction.hlsli): P[4].y = FrameResources::lightFunctions (0xFFFFFFFF: none);
// a point or spot light's in-scattering integrand carries its function toward each quadrature point, at the froxel's
// lateral size over the distance to the light (the function varies across the light's cone; the points sample it).
// Particle media (smoke, fire; E's volumeMedia, request 20260925_FX_particle_render_rules 3b), main view: P[4].x = the
// view's volumeSlices (RGBA16F gridX x gridY x 2S: slice s's media optical depth tau_p, then its self-attenuated source
// S_p in nits before exposure; 0xFFFFFFFF: none). In each slice air and media are mixed uniformly: with J the sources per
// unit optical depth, L = (J_a tau_a + J_p tau_p) g(tau_a + tau_p), g(x) = (1 - e^-x) / x; the slice's air alone is
// J_a tau_a g(tau_a) and S_p = J_p tau_p g(tau_p), so L = air g(tau_a + tau_p) / g(tau_a) + S_p g(tau_a + tau_p) / g(tau_p)
// (exact for a uniform mixture, the froxel's condition for both), and the slice's optical depth is tau_a + tau_p. Sky
// pixels multiply the far-field sky by the media transmittance to far_m (slice 3 (S + 1) + 1: media optical depth) and add
// the sky correction, which then holds per slice source' - A_lut e^-(tau_p,total - tau_p,before) (A_lut: the slice's air
// in-scattering the sky LUT has), so the sum is exact given the LUT's air: the LUT's air behind the media is attenuated
// by them, the air in front is not.
// Frame constants of the view (main, or a planar reflection view).
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Shadow/VsmAir.hlsli"
#include "Passes/Shadow/VsmLocalSample.hlsli"
#include "Passes/Lights/LightFunction.hlsli"

#define AIR_SHADOW_POINTS 24u
groupshared float3 gs_tau[64];
groupshared float3 gs_source[64];
groupshared float4 gs_hat[64];  // L / (1 - T) of node s + 1, .w = 1 where the node has air
groupshared float3 gs_sky[64];  // slice s's sky correction attenuated to the camera
groupshared uint gs_lastSky;    // 1 + the last slice the sky correction needs

float3 froxelSelfAttenuation(float3 x) { return select(x > 1e-4, (1 - exp(-x)) / max(x, 1e-4), 1 - 0.5 * x); }

static const float kGaussX[8] = { -0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
                                  0.1834346424956498, 0.5255324099163290, 0.7966664774136267, 0.9602898564975363 };
static const float kGaussW[8] = { 0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
                                  0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763 };

// Visibility of point p from local light slot 'slot' (hard test at the mip of texel <= width at p's face depth).
float airLocalShadow(VsmLocalResources r, VsmLocalLight l, uint slot, float3 p, float width, float biasTexels)
{
    const VsmLocalPoint q = vsmLocalProject(l, p);
    if (q.z <= l.nearM) return 1;
    float3 right, up, axis;
    vsmCubeBasis(q.face, right, up, axis);
    uint mu;
    const uint key = vsmLocalKeyAt(r, l, slot, axis + q.xy.x * right + q.xy.y * up, vsmLocalMip(width, q.z), mu);
    return key == VSM_EMPTY || -vsmDecode(key) >= q.z - biasTexels * 2 * q.z / vsmLocalRes(mu) ? 1.0 : 0.0;
}

// Air in-scattering of light l along o + dir t, t in [0, len] (nits), relative to the segment's start; shadowed by its
// VSM when slot != VSM_LOCAL_NONE.
float3 airLocalLight(GpuLight l, float3 o, float3 dir, float len, AirCoefficients c, float mieG, VsmLocalResources r, VsmLocalLight sl, uint slot,
                     float width, float biasTexels, uint functions, uint lightIndex, float lateral)
{
    const bool withFunction = functions != LIGHT_FUNCTION_NONE && (lightType(l) == LIGHT_POINT || lightType(l) == LIGHT_SPOT);
    const float tc = dot(l.position - o, dir);
    // Distance of the line from the light, not below the emitter's size (1 cm for points): the point-source integrand
    // is singular on the line.
    const float h = max(length(l.position - (o + dir * tc)), max(max(l.size.x, l.size.y), 0.01));
    const float th0 = atan(-tc / h), th1 = atan((len - tc) / h);
    const float mid = 0.5 * (th0 + th1), half = 0.5 * (th1 - th0);
    float3 sum = 0;
    const bool shadowed = slot != VSM_LOCAL_NONE;
    const uint points = shadowed ? AIR_SHADOW_POINTS : 8u;
    [loop] for (uint i = 0; i < points; ++i)
    {
        const float x = shadowed ? (i + 0.5) / AIR_SHADOW_POINTS * 2 - 1 : kGaussX[i];
        const float weight = shadowed ? 2.0 / AIR_SHADOW_POINTS : kGaussW[i];
        const float th = mid + half * x;
        const float t = tc + h * tan(th);
        const float3 v = o + dir * t - l.position;
        const float d = h / cos(th);
        const float3 w = v / max(length(v), 1e-6);
        const float nu = -sin(th);  // cosine between the light's propagation (w) and the path to the camera (-dir)
        const float3 phase = c.rayleigh * airRayleighPhase(nu) + c.mie * airMiePhase(nu, mieG);
        const float visible = shadowed ? airLocalShadow(r, sl, slot, o + dir * t, width, biasTexels) : 1.0;
        const float3 f = withFunction ? lightFunction(functions, lightIndex, l.forward, l.right, w, lateral / max(d, 1e-4), g_time) : 1.0;
        sum += visible * weight * froxelIntensity(l, w) * froxelWindow(l, d) * f * phase * exp(-c.extinction * max(t, 0.0));
    }
    return sum * (half / h) * l.color;
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint s : SV_GroupIndex)
{
    const FroxelGrid g = froxelGrid(P[0].x);
    const uint2 tile = gid.xy;
    const AtmosphereParams a = airParamsFromTexels(P[0].z);
    const uint tlut = P[0].z, mlut = P[0].w;
    const float3 ray = froxelTileRay(g, tile);
    const float toRay = length(ray);
    const float3 dir = ray / toRay;
    const float3 sun = normalize(g_sunDirection);
    const float3 E = g_sunIlluminance * g_sunColor;
    const float nu = dot(dir, sun);
    const float phaseR = airRayleighPhase(nu), phaseM = airMiePhase(nu, a.mieG);
    const float stepAltitude = asfloat(P[2].z);
    const uint experiment = P[2].w;
    float3 tau = 0, source = 0, skyTerm = 0;
    VsmAirWalkCount walk = (VsmAirWalkCount)0;  // statistics (P[3].w)
    const float tStart = airViewStart(g_clipPlane, g_cameraPosition, dir);
    const float zs0 = froxelNodeDepth(g, s), zs1 = froxelNodeDepth(g, s + 1);
    const bool hasAir = s < g.slices && zs1 * toRay > tStart;
    // Readers of this tile's nodes (3 x 3 tile neighbourhood).
    float zSurface = 3.0e38;
    bool skyRead = true;
    if (P[3].z != 0xFFFFFFFFu)
    {
        Texture2D<float2> readers = ResourceDescriptorHeap[P[3].z];
        zSurface = 0;
        skyRead = false;
        [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                const float2 r = readers[clamp(int2(tile) + int2(dx, dy), 0, int2(g.gridX, g.gridY) - 1)];
                zSurface = max(zSurface, r.x);
                skyRead = skyRead || r.y > 0;
            }
    }
    if (s == 0) gs_lastSky = 0;
    // Particle media of this slice (P[4].x) and their optical depth before it and to far_m (inclusive scan in gs_tau,
    // reused below).
    const bool media = P[4].x != 0xFFFFFFFFu;
    float3 mediaTau = 0, mediaSource = 0;
    if (media && s < g.slices)
    {
        Texture3D<float4> slices = ResourceDescriptorHeap[P[4].x];
        mediaTau = slices.Load(int4(tile, s, 0)).rgb;
        mediaSource = slices.Load(int4(tile, g.slices + s, 0)).rgb;
    }
    float3 mediaBefore = 0, mediaTotal = 0;
    if (media)
    {
        gs_tau[s] = mediaTau;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint dm = 1; dm < 64; dm <<= 1)
        {
            const float3 add = s >= dm ? gs_tau[s - dm] : 0;
            GroupMemoryBarrierWithGroupSync();
            gs_tau[s] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        mediaBefore = gs_tau[s] - mediaTau;
        mediaTotal = gs_tau[63];
    }
    GroupMemoryBarrierWithGroupSync();
    if (skyRead && hasAir)
    {
        ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].x];
        bool active = (lists.Load(g.headerBase + froxelIndex(g, tile, s) * 4) & 63u) != 0;
        if (P[1].w != 0xFFFFFFFFu)
        {
            ConstantBuffer<VsmConstants> vcs = ResourceDescriptorHeap[P[1].w];
            const float t0 = max(zs0 * toRay, tStart), t1 = zs1 * toRay;
            const float h0 = dot(g_cameraPosition + dir * t0, vcs.lightZ), h1 = dot(g_cameraPosition + dir * t1, vcs.lightZ);
            active = active || min(h0, h1) < vcs.hMax;
        }
        active = active || any(mediaTau > 0);  // sky pixels behind the media
        if (active) InterlockedMax(gs_lastSky, s + 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (hasAir && (zs0 < zSurface || s < gs_lastSky))
    {
        const float z0 = zs0, z1 = zs1;
        const float t0 = max(z0 * toRay, tStart), len = z1 * toRay - t0;
        const float3 o = g_cameraPosition + dir * t0;
        // Substeps: the air's density is exponential in altitude; midpoint steps of at most stepAltitude.
        const float h0 = airAltitude(a, o), h1 = airAltitude(a, o + dir * len), hm = airAltitude(a, o + dir * (0.5 * len));
        const float dh = max(max(abs(h1 - h0), abs(hm - h0)), abs(hm - h1));
        const uint steps = (experiment & 4) ? 0u : (uint)clamp(ceil(dh / stepAltitude), 1.0, 32.0);
        const float dt = len / steps;
        float3 single = 0, multi = 0;
        [loop] for (uint k = 0; k < steps; ++k)
        {
            const float3 p = airLiftToSurface(a, o + dir * ((k + 0.5) * dt));
            const AirCoefficients c = airCoefficients(a, max(0.0, airAltitude(a, p)));
            const float3 w = exp(-tau) * airIntegral(c.extinction, dt);
            single += w * (c.rayleigh * phaseR + c.mie * phaseM) * ((experiment & 8) ? 1.0 : airSunTransmittance(a, tlut, p, sun));
            multi += w * (c.rayleigh + c.mie) * ((experiment & 16) ? 1.0 : airMultipleScattering(a, mlut, p, dir, sun));
            tau += c.extinction * dt;
        }
        // Casters' shadows in the air: the shadowed fraction of the segment removes that part of the single scattering.
        float f = 0;
        if (P[1].w != 0xFFFFFFFFu && any(single > 0) && (experiment & 1) == 0)
        {
            VsmResources r;
            r.table = ResourceDescriptorHeap[P[1].x];
            r.pool = ResourceDescriptorHeap[P[1].y];
            r.blocks = ResourceDescriptorHeap[P[1].z];
            r.searchBound = ResourceDescriptorHeap[P[2].x];
            r.cbv = P[1].w;
            ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[P[1].w];
            uint k;
            if (vsmAirLevel(vc, froxelTileWidth(g, 0.5 * (z0 + z1)), asfloat(P[2].y), k))
            {
                f = vsmAirShadowFraction(r, o, o + dir * len, k, walk, (experiment & 32) != 0);
                ++walk.slices;
            }
        }
        source = E * (single * (1 - f) + multi);
        skyTerm = -E * single * f;  // what the sky LUT has and the shadows remove
        // Local lights of the froxel's list (air at the segment's midpoint).
        const float3 pm = airLiftToSurface(a, o + dir * (0.5 * len));
        const AirCoefficients cm = airCoefficients(a, max(0.0, airAltitude(a, pm)));
        ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].x];
        const uint h = lists.Load(g.headerBase + froxelIndex(g, tile, s) * 4);
        const uint first = h >> 6, count = (experiment & 2) ? 0u : h & 63u;
        VsmLocalResources lr;
        lr.table = ResourceDescriptorHeap[P[1].x];
        lr.pool = ResourceDescriptorHeap[P[1].y];
        lr.blocks = ResourceDescriptorHeap[P[1].z];
        const bool localShadows = P[3].x != 0xFFFFFFFFu && P[1].w != 0xFFFFFFFFu;
        const float width = froxelTileWidth(g, 0.5 * (z0 + z1)) / asfloat(P[2].y);
        float biasTexels = 1;
        if (P[1].w != 0xFFFFFFFFu)
        {
            ConstantBuffer<VsmConstants> vcl = ResourceDescriptorHeap[P[1].w];
            biasTexels = vcl.receiverBiasTexels;
        }
        for (uint i = 0; i < count; ++i)
        {
            const uint w = lists.Load(g.indexBase + ((first + i) >> 1) * 4);
            const uint entry = ((first + i) & 1) ? w >> 16 : w & 0xFFFFu, li = entry & 0x7FFFu;
            uint slot = VSM_LOCAL_NONE;
            VsmLocalLight sl = (VsmLocalLight)0;
            if (localShadows && (entry & 0x8000u) != 0)
            {
                StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[P[3].y];
                StructuredBuffer<VsmLocalLight> locals = ResourceDescriptorHeap[P[3].x];
                slot = slotOf[li];
                if (slot != VSM_LOCAL_NONE) sl = locals[slot];
            }
            const float3 local = airLocalLight(loadLight(li), o, dir, len, cm, a.mieG, lr, sl, slot, width, biasTexels, P[4].y, li,
                                               froxelTileWidth(g, 0.5 * (z0 + z1)));
            source += local;
            skyTerm += local;
        }
    }
    if (media)
    {
        const float3 lut = source - skyTerm;  // the slice's air in-scattering as the sky LUT holds it
        if (any(mediaTau > 0))
        {
            const float3 mixed = froxelSelfAttenuation(tau + mediaTau);
            source = source * mixed / froxelSelfAttenuation(tau) + mediaSource * mixed / froxelSelfAttenuation(mediaTau);
            tau += mediaTau;
        }
        // Every slice (air in front of the media too): the reader attenuates the whole LUT sky by the media to far_m.
        skyTerm = source - lut * exp(-(mediaTotal - mediaBefore));
    }
    // Exclusive scan of the optical depth, inclusive scan of the attenuated sources (Hillis-Steele over 64 slices).
    gs_tau[s] = tau;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint d = 1; d < 64; d <<= 1)
    {
        const float3 add = s >= d ? gs_tau[s - d] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_tau[s] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    gs_source[s] = exp(-(gs_tau[s] - tau)) * source;
    gs_sky[s] = exp(-(gs_tau[s] - tau)) * skyTerm;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint d2 = 1; d2 < 64; d2 <<= 1)
    {
        const float3 add = s >= d2 ? gs_source[s - d2] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_source[s] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    // L / (1 - T) per node; nodes without air take the first node that has it.
    {
        const float3 tauN = gs_tau[s];
        const bool air = s < g.slices && all(tauN > 0);
        gs_hat[s] = air ? float4(gs_source[s] / airOneMinusExp(tauN), 1) : float4(0, 0, 0, 0);
    }
    GroupMemoryBarrierWithGroupSync();
    float3 hat = gs_hat[s].xyz;
    if (gs_hat[s].w == 0)
        for (uint j = s + 1; j < g.slices; ++j)
            if (gs_hat[j].w != 0)
            {
                hat = gs_hat[j].xyz;
                break;
            }
    float3 hat0 = 0;
    if (s == 0)
        for (uint j0 = 0; j0 < g.slices; ++j0)
            if (gs_hat[j0].w != 0)
            {
                hat0 = gs_hat[j0].xyz;
                break;
            }
    if (P[3].w != 0xFFFFFFFFu && WaveActiveAnyTrue(walk.capped != 0) && WaveIsFirstLane())  // a walk's hard cap (INTERFACES 3.6)
    {
        RWByteAddressBuffer st = ResourceDescriptorHeap[P[3].w];
        st.InterlockedOr(VSM_STATS_ERROR_BYTE, VSM_ERR_AIR_WALK);
    }
    if (P[3].w != 0xFFFFFFFFu && (P[2].w & 0x10000u) != 0)  // walk statistics (measurement only): one atomic per wave and counter
    {
        const uint slices = WaveActiveSum(walk.slices), mixed = WaveActiveSum(walk.mixedPages > 0 ? 1u : 0u);
        const uint b32 = WaveActiveSum(walk.blocks32), b8 = WaveActiveSum(walk.blocks8), texels = WaveActiveSum(walk.texels);
        if (WaveIsFirstLane())
        {
            RWByteAddressBuffer st = ResourceDescriptorHeap[P[3].w];
            st.InterlockedAdd(80, slices);
            st.InterlockedAdd(84, mixed);
            st.InterlockedAdd(88, b32);
            st.InterlockedAdd(92, b8);
            st.InterlockedAdd(96, texels);
        }
    }
    if (s < g.slices)
    {
        // Node s + 1 of the three parts; thread 0 also writes node 0 (the camera).
        RWTexture3D<float4> volume = ResourceDescriptorHeap[P[0].y];
        const uint N = g.slices + 1;
        const float tn = froxelNodeDepth(g, s + 1) * toRay;
        float3 node = g_cameraPosition + dir * tn;
        if (tn < tStart) node = airMirror(g_clipPlane, node);  // before the mirror: the real path's point
        volume[uint3(tile, s + 1)] = float4(min(hat * g_exposure, 65504.0), 0);  // pre-exposed: fp16 precision follows the display
        volume[uint3(tile, N + s + 1)] = float4(gs_tau[s], 0);
        volume[uint3(tile, 2 * N + s + 1)] = float4(airSunTransmittance(a, tlut, airLiftToSurface(a, node), sun), 0);
        if (s == 0)
        {
            volume[uint3(tile, 0)] = float4(min(hat0 * g_exposure, 65504.0), 0);
            volume[uint3(tile, N)] = 0;
            const float3 camera = tStart > 0 ? airMirror(g_clipPlane, g_cameraPosition) : g_cameraPosition;
            volume[uint3(tile, 2 * N)] = float4(airSunTransmittance(a, tlut, airLiftToSurface(a, camera), sun), 0);
            float3 sky = 0;
            for (uint j = 0; j < g.slices; ++j) sky += gs_sky[j];
            volume[uint3(tile, 3 * N)] = float4(clamp(sky * g_exposure, -65504.0, 65504.0), 0);
            volume[uint3(tile, 3 * N + 1)] = float4(mediaTotal, 0);  // sky pixels: the media's transmittance to far_m
        }
    }
}
