// unx-kernel: cs_6_6 main
// unx-variants: QUEUED=0,1
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
//    projected intensity (froxelIntensity). A light with a shadow slot (list entry bit 15) shadows its air: the segment
//    is walked through the light's VSM texels at the mip whose texel matches the froxel's lateral resolution
//    (VsmLocalAirWalk.hlsli: exact texel-boundary walk over the page and block key ranges; lit where no page holds it;
//    VsmLocalMarkAir requests the pages the same walk crosses), which gives the lit set exactly; the same 8 Gauss nodes
//    then integrate the rule's interpolant over the lit set only (node weights from the lit set's Legendre moments);
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
// the page level, 64 local lights' air shadows; bit 16: walk statistics on, also the local lights' air walk: words 54..57
// entries walked, cells, largest cells of one entry, lit runs; 58, 59 over waves: the sum of the lane maxima of cells
// and of entries)
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
#include "Passes/Atmosphere/FroxelSlice.hlsli"
groupshared float3 gs_tau[64];
groupshared float3 gs_source[64];
groupshared float4 gs_hat[64];  // L / (1 - T) of node s + 1, .w = 1 where the node has air
groupshared float3 gs_sky[64];  // slice s's sky correction attenuated to the camera
groupshared uint gs_lastSky;    // 1 + the last slice the sky correction needs
groupshared uint gs_scan[64];   // shadowed local-light items of the slices (inclusive scan)
groupshared uint gs_item[64];   // a batch's items: slice | list position << 6
groupshared float gs_moments[64][8];  // each lane's piece of a shadowed item: its lit set's Legendre moments
groupshared uint gs_runs[64];         // and its lit runs | 0x80000000 unless fully lit

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
    AirLocalCount localWalk = (AirLocalCount)0;
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
        bool active = lists.Load(g.headerBase + froxelIndex(g, tile, s) * 8 + 4) != 0;
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
    // Local lights with a shadow slot are walked as items after the slice's own work (below); the slice keeps its list.
    const bool localShadows = P[3].x != 0xFFFFFFFFu && P[1].w != 0xFFFFFFFFu && (experiment & 64) == 0;
    float biasTexels = 1;
    if (P[1].w != 0xFFFFFFFFu)
    {
        ConstantBuffer<VsmConstants> vcl = ResourceDescriptorHeap[P[1].w];
        biasTexels = vcl.receiverBiasTexels;
    }
    VsmLocalResources lr;
    lr.table = ResourceDescriptorHeap[P[1].x];
    lr.pool = ResourceDescriptorHeap[P[1].y];
    lr.blocks = ResourceDescriptorHeap[P[1].z];
    uint myFirst = 0, myCount = 0, myShadowed = 0;
    uint2 myReach = 0;  // list positions (bit i of 64) of the shadowed lights whose range the slice's segment enters
    if (hasAir && (zs0 < zSurface || s < gs_lastSky))
    {
#if QUEUED
        ByteAddressBuffer air = ResourceDescriptorHeap[P[4].w];
        const FroxelAirResult integrated = froxelLoadAir(air, froxelIndex(g, tile, s));
#else
        const FroxelAirResult integrated = froxelAirSlice(g, tile, s);
#endif
        tau = integrated.tau; source = integrated.source; skyTerm = integrated.sky; walk = integrated.walk;
        const float z0 = zs0, z1 = zs1;
        const float t0 = max(z0 * toRay, tStart), len = z1 * toRay - t0;
        const float3 o = g_cameraPosition + dir * t0;
        // Local lights of the froxel's list (air at the segment's midpoint).
        const float3 pm = airLiftToSurface(a, o + dir * (0.5 * len));
        const AirCoefficients cm = airCoefficients(a, max(0.0, airAltitude(a, pm)));
        ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].x];
        const uint2 h = lists.Load2(g.headerBase + froxelIndex(g, tile, s) * 8);
        myFirst = h.x;
        myCount = (experiment & 2) ? 0u : h.y;
        const float width = froxelTileWidth(g, 0.5 * (z0 + z1)) / asfloat(P[2].y);
        for (uint i = 0; i < myCount; ++i)
        {
            uint li;
            const uint slot = airLocalSlot(lists, g, myFirst + i, localShadows, li);
            // A light whose range the segment's line never enters (distance h >= range; the rule's nodes lie at h / cos
            // >= h, where the window is 0) adds exactly 0: neither evaluated nor walked. (The froxel lists hold the lights
            // whose sphere meets the froxel, about a tenth of the depth beside the tile's centre ray.)
            const GpuLight light = loadLight(li);
            if (airLocalMap(light, o, dir, len).h >= light.range) continue;
            if (slot != VSM_LOCAL_NONE)
            {
                if (i < 32) myReach.x |= 1u << i;
                else myReach.y |= 1u << (i - 32);
                ++myShadowed;
                continue;
            }
            const float3 local = airLocalLight(light, o, dir, len, cm, a.mieG, P[4].y, li, froxelTileWidth(g, 0.5 * (z0 + z1)));
            source += local;
            skyTerm += local;
        }
    }
    // Shadowed local lights: the group's (slice, light) items, each walked by K lanes in K pieces uniform in the light's
    // angle (K = 64 / items when there are fewer than 64 items, else 1: one walk per lane left most lanes waiting for the
    // longest walk - 9 % lane use at fp_1000 [measured]). The pieces tile the segment exactly and the lit set's moments
    // add, so each item's first lane sums its pieces in order and evaluates the rule once. Each slice then adds its
    // items' results in list order (deterministic). Experiment bit 128: one lane per item (the pieces' reference in
    // FroxelTests).
    gs_scan[s] = myShadowed;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint ds = 1; ds < 64; ds <<= 1)
    {
        const uint add = s >= ds ? gs_scan[s - ds] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_scan[s] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint itemBase = gs_scan[s] - myShadowed, items = gs_scan[63];
    const uint K = (items >= 64 || (experiment & 128) != 0) ? 1u : 64u / max(items, 1u), B = 64u / K;  // lanes per item, items per batch
    const uint item = s / K, piece = s - item * K;  // item of the batch, piece
    [loop] for (uint b = 0; b < items; b += B)
    {
        ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].x];
        const bool mine = myShadowed > 0 && itemBase < b + B && itemBase + myShadowed > b;
        if (mine)
        {
            uint k = itemBase;
            for (uint i = 0; i < myCount && k < b + B; ++i)
            {
                if (((i < 32 ? myReach.x >> i : myReach.y >> (i - 32)) & 1u) == 0) continue;  // (the counted items)
                if (k >= b) gs_item[k - b] = s | i << 6;
                ++k;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        const bool valid = item < B && b + item < items;
        float z0 = 0, z1 = 0, len = 0;
        float3 o = 0;
        uint li = 0;
        GpuLight l = (GpuLight)0;
        AirLocalMap mp = (AirLocalMap)0;
        if (valid)
        {
            const uint it = gs_item[item], is = it & 63u;
            z0 = froxelNodeDepth(g, is);
            z1 = froxelNodeDepth(g, is + 1);
            const float t0 = max(z0 * toRay, tStart);
            len = z1 * toRay - t0;
            o = g_cameraPosition + dir * t0;
            const uint h = lists.Load(g.headerBase + froxelIndex(g, tile, is) * 8);  // the run's first entry
            const uint slot = airLocalSlot(lists, g, h + (it >> 6), localShadows, li);
            l = loadLight(li);
            mp = airLocalMap(l, o, dir, len);
            StructuredBuffer<VsmLocalLight> locals = ResourceDescriptorHeap[P[3].x];
            const VsmLocalLight sl = locals[slot];
            const float ta = airLocalPieceT(mp, len, piece, K), tb = airLocalPieceT(mp, len, piece + 1, K);
            VsmLocalAirResult lit = (VsmLocalAirResult)0;
            lit.fullyLit = true;  // an empty piece (tb <= ta) leaves the item's lit set as the other pieces make it
            if (tb > ta)
                lit = vsmLocalAirLit(lr, sl, slot, o + dir * ta - sl.position, dir, tb - ta, froxelTileWidth(g, 0.5 * (z0 + z1)) / asfloat(P[2].y),
                                     biasTexels, mp.tc - ta, mp.h, mp.mid, mp.half);
            [unroll] for (uint q = 0; q < 8; ++q) gs_moments[s][q] = lit.m[q];
            gs_runs[s] = lit.litRuns | (lit.fullyLit ? 0u : 0x80000000u);
            localWalk.cells += lit.steps;
            localWalk.maxCells = max(localWalk.maxCells, lit.steps);
            localWalk.capped |= lit.capped ? 1u : 0u;
        }
        GroupMemoryBarrierWithGroupSync();
        if (valid && piece == 0)
        {
            float m[8] = (float[8])0;
            uint runs = 0;
            bool partial = false;
            for (uint p2 = 0; p2 < K; ++p2)
            {
                [unroll] for (uint q = 0; q < 8; ++q) m[q] += gs_moments[s + p2][q];
                runs += gs_runs[s + p2] & 0x7FFFFFFFu;
                partial = partial || (gs_runs[s + p2] >> 31) != 0;
            }
            ++localWalk.entries;
            localWalk.runs += runs;
            const float3 pm = airLiftToSurface(a, o + dir * (0.5 * len));
            const AirCoefficients cm = airCoefficients(a, max(0.0, airAltitude(a, pm)));
            gs_source[item] = runs == 0 ? 0.0 : airLocalEval(l, o, dir, mp, cm, a.mieG, P[4].y, li, froxelTileWidth(g, 0.5 * (z0 + z1)), partial, m);
        }
        GroupMemoryBarrierWithGroupSync();
        if (mine)
            for (uint k2 = max(itemBase, b); k2 < min(itemBase + myShadowed, b + B); ++k2)
            {
                source += gs_source[k2 - b];
                skyTerm += gs_source[k2 - b];
            }
        GroupMemoryBarrierWithGroupSync();
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
    if (P[3].w != 0xFFFFFFFFu && WaveActiveAnyTrue(localWalk.capped != 0) && WaveIsFirstLane())
    {
        RWByteAddressBuffer st = ResourceDescriptorHeap[P[3].w];
        st.InterlockedOr(VSM_STATS_ERROR_BYTE, VSM_ERR_LOCAL_AIR_WALK);
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
        const uint le = WaveActiveSum(localWalk.entries), lc = WaveActiveSum(localWalk.cells), lm = WaveActiveMax(localWalk.maxCells), lr = WaveActiveSum(localWalk.runs);
        const uint wc = WaveActiveMax(localWalk.cells), we = WaveActiveMax(localWalk.entries);  // the wave runs its slowest lane
        if (WaveIsFirstLane())
        {
            RWByteAddressBuffer st = ResourceDescriptorHeap[P[3].w];
            st.InterlockedAdd(216, le);
            st.InterlockedAdd(220, lc);
            st.InterlockedMax(224, lm);
            st.InterlockedAdd(228, lr);
            st.InterlockedAdd(232, wc);
            st.InterlockedAdd(236, we);
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
