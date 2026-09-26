// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2
// Debug drawing tests (DebugTests.cpp). Frame constants b1 carry g_debugDraw.
//   MODE 0: fill P[0].x (RWTexture2D<float4>) of P[0].y x P[0].z with asfloat(P[1].xyzw)
//   MODE 1: GPU appends: a white screen line (10, 100)-(60, 100) of width 1 and the number 12.5 at pixel (80, 100)
//   MODE 2: P[0].x screen lines appended (overflow)
#include "Bindless.hlsli"
#include "Passes/Debug/DebugDraw.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
#if MODE == 0
    if (id.x >= P[0].y || id.y >= P[0].z) return;
    RWTexture2D<float4> target = ResourceDescriptorHeap[P[0].x];
    target[id.xy] = asfloat(P[1]);
#elif MODE == 1
    if (any(id != 0)) return;
    debugLine(float3(10, 100, 0), float3(60, 100, 0), float4(1, 1, 1, 1), 1.0f, DEBUG_SCREEN);
    debugNumber(float3(80, 100, 0), float2(0, 0), 12.5f, 16.0f, float4(1, 1, 1, 1), DEBUG_SCREEN);
#else
    if (id.y != 0 || id.z != 0 || id.x >= P[0].x) return;
    debugLine(float3(5, 5 + id.x, 0), float3(20, 5 + id.x, 0), float4(1, 1, 0, 1), 1.0f, DEBUG_SCREEN);
#endif
}
