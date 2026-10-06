#ifndef UNX_HIT_LIGHTING_QUEUE_HLSLI
#define UNX_HIT_LIGHTING_QUEUE_HLSLI
// A minimal trace can publish its exact intersection for a later lighting pass.
// Header: {count, reserved x3}; each 80-byte record: original dispatch identity,
// reserved x3, the eight RtHit words, then the two float4 halves of RayDesc.
// No ray is retraced, dropped, quantized or assigned a different random seed.
#define HIT_LIGHTING_HEADER_BYTES 16u
#define HIT_LIGHTING_RECORD_BYTES 80u

void hitLightingEnqueue(uint descriptor, uint source, RtHit hit, RayDesc ray, uint3 context = uint3(0, 0, 0))
{
    RWByteAddressBuffer queue = ResourceDescriptorHeap[descriptor];
    uint first = 0;
    const uint count = WaveActiveCountBits(true);
    if (WaveIsFirstLane()) queue.InterlockedAdd(0, count, first);
    const uint index = WaveReadLaneFirst(first) + WavePrefixCountBits(true);
    const uint base = HIT_LIGHTING_HEADER_BYTES + index * HIT_LIGHTING_RECORD_BYTES;
    queue.Store4(base, uint4(source, context));
    queue.Store4(base + 16, uint4(asuint(hit.t), hit.instance, hit.geometry, hit.primitive));
    queue.Store4(base + 32, uint4(asuint(hit.barycentrics), hit.frontFace, hit.pad));
    queue.Store4(base + 48, asuint(float4(ray.Origin, ray.TMin)));
    queue.Store4(base + 64, asuint(float4(ray.Direction, ray.TMax)));
}

void hitLightingLoadContext(uint descriptor, uint index, out uint source, out RtHit hit, out RayDesc ray, out uint3 context)
{
    ByteAddressBuffer queue = ResourceDescriptorHeap[descriptor];
    const uint base = HIT_LIGHTING_HEADER_BYTES + index * HIT_LIGHTING_RECORD_BYTES;
    const uint4 identity = queue.Load4(base);
    source = identity.x; context = identity.yzw;
    const uint4 lo = queue.Load4(base + 16), hi = queue.Load4(base + 32);
    hit.t = asfloat(lo.x); hit.instance = lo.y; hit.geometry = lo.z; hit.primitive = lo.w;
    hit.barycentrics = asfloat(hi.xy); hit.frontFace = hi.z; hit.pad = hi.w;
    const float4 origin = asfloat(queue.Load4(base + 48)), direction = asfloat(queue.Load4(base + 64));
    ray.Origin = origin.xyz; ray.TMin = origin.w;
    ray.Direction = direction.xyz; ray.TMax = direction.w;
}
void hitLightingLoad(uint descriptor, uint index, out uint source, out RtHit hit, out RayDesc ray)
{
    uint3 context;
    hitLightingLoadContext(descriptor, index, source, hit, ray, context);
}
#endif
