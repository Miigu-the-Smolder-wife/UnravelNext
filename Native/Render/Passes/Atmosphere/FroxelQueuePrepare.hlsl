// unx-kernel: cs_6_6 main
// One tile group; append every required slice once, never truncate. Queue
// capacity equals gridX*gridY*slices. P4.z = queue UAV, P4.w = air scratch UAV.
#include "Passes/Atmosphere/FroxelSlice.hlsli"
groupshared float3 gs_tau[64];
groupshared uint gs_lastSky;
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
    if (s >= g.slices) return;
    const uint index = froxelIndex(g, tile, s);
    RWByteAddressBuffer air = ResourceDescriptorHeap[P[4].w];
    froxelStoreAir(air, index, (FroxelAirResult)0);
    if (hasAir && (zs0 < zSurface || s < gs_lastSky))
    {
        RWByteAddressBuffer queue = ResourceDescriptorHeap[P[4].z];
        uint at;
        queue.InterlockedAdd(0, 1, at);
        queue.Store(16 + at * 4, index);
    }
}
