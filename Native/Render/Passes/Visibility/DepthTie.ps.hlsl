// unx-kernel: ps_6_6 main
// unx-variants: MODE=0,1 ALPHA=0,1
// Replay against the immutable final depth. MODE0 chooses a stable primitive
// identity, MODE1 writes only that winner's vis id. No depth perturbation, ROV,
// quantisation, sample loss or 128-bit publication required.
// VisRaster.ms constants plus P[2]: winners, depth, width, unused;
// P[3].x the prior vis target (MODE0 only, excludes sky/planar-mask pixels).
#include "Passes/Visibility/AlphaTest.hlsli"
#include "PrimitiveOrder.hlsli"

#if MODE == 0
void main(float4 position : SV_Position,
#else
uint main(float4 position : SV_Position,
#endif
#if ALPHA
          float2 uv : TEXCOORD0, nointerpolation uint material : MATERIAL,
#endif
          nointerpolation uint visId : VISID)
#if MODE == 1
          : SV_Target0
#endif
{
#if ALPHA
    if (!alphaTestCoveredAt(material, uv, position.xy)) discard;
#endif
    const uint2 pixel = uint2(position.xy);
    Texture2D<float> depth = ResourceDescriptorHeap[P[2].y];
    if (asuint(position.z) != asuint(depth[pixel])) discard;
    const uint address = (pixel.y * P[2].z + pixel.x) * 8;
    const uint64_t key = primitiveOrder(P[0].x, visId);
#if MODE == 0
    Texture2D<uint> priorVis = ResourceDescriptorHeap[P[3].x];
    if (priorVis[pixel] == VIS_NONE) discard;
    RWByteAddressBuffer winners = ResourceDescriptorHeap[P[2].x];
    uint64_t previous;
    winners.InterlockedMin64(address, key, previous);
#else
    ByteAddressBuffer winners = ResourceDescriptorHeap[P[2].x];
    const uint2 words = winners.Load2(address);
    if (key != ((uint64_t)words.x | ((uint64_t)words.y << 32))) discard;
    return visId;
#endif
}
