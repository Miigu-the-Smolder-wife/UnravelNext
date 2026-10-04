#ifndef UNX_TSR_REJECT_CODES_HLSLI
#define UNX_TSR_REJECT_CODES_HLSLI
// Lossless code-space operations shared by the original rejection kernel and
// the fused prefix. Arithmetic still runs in the original measurement space.
uint pack(float3 c)
{
    const float3 s = sqrt(saturate(c));
    return (uint)(s.r * 2047.0 + 0.5) | ((uint)(s.g * 2047.0 + 0.5) << 11) | ((uint)(s.b * 1023.0 + 0.5) << 22);
}
float3 unpack(uint v)
{
    const float3 s = float3(v & 2047u, (v >> 11) & 2047u, v >> 22) * float3(1.0 / 2047.0, 1.0 / 2047.0, 1.0 / 1023.0);
    return s * s;
}
uint3 colourCodes(uint v) { return uint3(v & 2047u, (v >> 11) & 2047u, v >> 22); }
uint packCodes(uint3 v) { return v.x | (v.y << 11) | (v.z << 22); }
#endif
