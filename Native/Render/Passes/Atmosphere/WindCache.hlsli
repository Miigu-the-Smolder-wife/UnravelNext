// The wind for GPU consumers (B6; Docs/Design/Requests/20260926_B_weather_fields.md 1.3): the frame's wind header
// (FrameResources::wind, raw SRV, 48 B) describes S's cache -- windAt (WindField.hlsli) evaluated at the centres of a
// 64^3 grid of 8 m cells around the camera, RGBA16F (FrameResources::windCache: declare it SrvCompute / SrvGraphics in
// the pass that samples it) -- and the tick's records for the exact formula.
//   windSample(header, x)  trilinear in the cache: the field's band >= 16 m (two cells); outside the grid the nearest
//                          edge value; 0 without a header.
//   windExact(header, x)   windAt from the records: the full turbulence (particles, cloth, hair); cost = records x noise.
// Header: { reference xyz (the records' origins are relative to it), time s, grid origin xyz, spacing m, texture SRV,
// records SRV, record count, grid cells per axis }.
#ifndef UNX_ATMOSPHERE_WIND_CACHE_HLSLI
#define UNX_ATMOSPHERE_WIND_CACHE_HLSLI
#include "Bindless.hlsli"
#include "Passes/Atmosphere/WindField.hlsli"

struct WindHeader
{
    float3 reference;
    float time;
    float3 gridOrigin;
    float spacing;
    uint textureSrv, recordsSrv, recordCount, cells;
};

WindHeader windHeader(uint srv)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    WindHeader h;
    const float4 w0 = asfloat(b.Load4(0)), w1 = asfloat(b.Load4(16));
    const uint4 w2 = b.Load4(32);
    h.reference = w0.xyz;
    h.time = w0.w;
    h.gridOrigin = w1.xyz;
    h.spacing = w1.w;
    h.textureSrv = w2.x;
    h.recordsSrv = w2.y;
    h.recordCount = w2.z;
    h.cells = w2.w;
    return h;
}

float3 windSample(uint headerSrv, float3 worldPos)
{
    if (headerSrv == 0xFFFFFFFFu) return 0;  // no wind header this frame
    const WindHeader h = windHeader(headerSrv);
    Texture3D<float4> cache = ResourceDescriptorHeap[h.textureSrv];
    const float3 uvw = (worldPos - h.gridOrigin) / (h.spacing * h.cells);  // texel centres at (i + 0.5) / cells
    return cache.SampleLevel(g_linearClamp, uvw, 0).xyz;
}

float3 windExact(uint headerSrv, float3 worldPos)
{
    if (headerSrv == 0xFFFFFFFFu) return 0;  // no wind header this frame
    const WindHeader h = windHeader(headerSrv);
    StructuredBuffer<WindRecord> records = ResourceDescriptorHeap[h.recordsSrv];
    return windAt(records, h.recordCount, worldPos - h.reference, h.time);
}
#endif
