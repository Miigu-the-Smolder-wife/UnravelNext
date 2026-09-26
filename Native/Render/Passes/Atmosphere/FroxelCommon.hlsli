// Froxel grid of the main view (ARCHITECTURE 2.3, 2.4; INTERFACES 7.4): layout and helpers shared by S's froxel kernels
// and the public lookups (Froxel.hlsli). Owner: S.
//
// Grid: screen tiles of tilePx pixels x `slices` depth slices, exponential in view depth between nearM and farM
// (slice s spans z_s = near (far / near)^(s / S) .. z_(s+1)); the last slice extends to infinity for light lists.
//
// FrameResources::froxelLights is one raw buffer:
//   bytes [0, 64)        FroxelGrid (below)
//   bytes [64, ...)      one uint per froxel: first index entry << 6 | count (count <= lights_max <= 32)
//   bytes [indexBase, ..) light indices, 16 bit, two per uint; froxel f's run starts at entry f x indexStride (lights_max
//                        rounded up to even): every list fits (no allocation, no overflow; the bytes a frame touches
//                        are the lights actually listed)
// Froxel index = (slice * gridY + tileY) * gridX + tileX. Lists are ordered by importance at the froxel centre
// (descending), so the first shading.analytic_lights_max are the strongest.
#ifndef UNX_FROXEL_COMMON_HLSLI
#define UNX_FROXEL_COMMON_HLSLI
#include "Frame.hlsli"
#include "Scene.hlsli"

struct FroxelGrid
{
    uint gridX, gridY, slices, tilePx;
    float nearM, farM, logRatio, pad0;       // logRatio = log2(far / near)
    uint headerBase, indexBase, indexStride, indexCount;    // byte offsets; entries per froxel; entries listed this frame
    uint overflowLists, droppedLights, maxCount, candidateOverflow;  // statistics: lists truncated at lights_max, light
                                                                     // entries dropped by truncation, largest count before
                                                                     // truncation, tiles whose frustum held more lights
                                                                     // than FROXEL_CANDIDATES
};

FroxelGrid froxelGrid(uint lightsBuffer)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[lightsBuffer];
    return b.Load<FroxelGrid>(0);
}

// Continuous slice coordinate of a view depth: 0 at nearM, `slices` at farM.
float froxelSliceCoord(FroxelGrid g, float linearDepth) { return log2(max(linearDepth, 1e-6) / g.nearM) / g.logRatio * g.slices; }
uint froxelSlice(FroxelGrid g, float linearDepth) { return (uint)clamp(floor(froxelSliceCoord(g, linearDepth)), 0.0, float(g.slices - 1)); }
float froxelSliceDepth(FroxelGrid g, float s) { return g.nearM * exp2(g.logRatio * s / g.slices); }
uint froxelIndex(FroxelGrid g, uint2 tile, uint slice) { return (slice * g.gridY + tile.y) * g.gridX + tile.x; }

// Depth of node n = 0..S: the camera, then the slice boundaries (node S = farM). Slice s spans nodes s .. s + 1 (slice 0
// starts at the camera).
float froxelNodeDepth(FroxelGrid g, uint n) { return n == 0 ? 0.0 : froxelSliceDepth(g, float(n)); }

// Main view: camera forward, and the world-space ray through the centre of a tile scaled to unit view depth (the point
// at view depth z is g_cameraPosition + ray * z). Tile centres follow the lookup (texel x <-> pixel (x + 0.5) tilePx).
float3 froxelForward() { return -normalize(g_view[2].xyz); }
float3 froxelRayAt(float2 pixel)
{
    const float2 ndc = float2(pixel.x / g_viewWidth * 2 - 1, 1 - pixel.y / g_viewHeight * 2);
    const float4 p = mul(g_invViewProj, float4(ndc, 1, 1));  // device depth 1 = view depth g_nearPlane
    return (p.xyz / p.w - g_cameraPosition) / g_nearPlane;
}
float3 froxelTileRay(FroxelGrid g, uint2 tile) { return froxelRayAt((float2(tile) + 0.5) * g.tilePx); }
// Lateral width of a tile at view depth z (m).
float froxelTileWidth(FroxelGrid g, float z) { return g.tilePx * 2 * z * g_tanHalfFovY / g_viewHeight; }

// ---- Lights (INTERFACES 8.2): bounding sphere, intensity towards a point, spot window.
float froxelLightRadius(GpuLight l)
{
    const float extent = lightType(l) == LIGHT_TUBE ? 0.5 * l.size.x + l.size.y
                       : (lightType(l) == LIGHT_RECT ? 0.5 * length(l.size) : (lightType(l) == LIGHT_DISK || lightType(l) == LIGHT_SPHERE ? l.size.x : 0.0));
    return l.range + extent;
}
// Distance window w(d) = saturate(1 - (d / range)^4)^2.
float froxelWindow(GpuLight l, float d)
{
    const float x = d / max(l.range, 1e-6);
    const float x2 = x * x;
    const float w = saturate(1 - x2 * x2);
    return w * w;
}
// Largest luminous intensity of the light over directions (candela; area lights: luminance x largest projected area).
float froxelPeakIntensity(GpuLight l)
{
    const uint t = lightType(l);
    if (t == LIGHT_POINT || t == LIGHT_SPOT) return l.intensity;
    if (t == LIGHT_RECT) return l.intensity * l.size.x * l.size.y;
    if (t == LIGHT_DISK || t == LIGHT_SPHERE) return l.intensity * 3.14159265 * l.size.x * l.size.x;
    return l.intensity * (2 * l.size.y * l.size.x + 3.14159265 * l.size.y * l.size.y);
}
// Luminous intensity of the light towards a point in direction 'toPoint' (unit, from the light), in candela.
// Area lights: luminance x projected area (a point-source intensity; exact far from the emitter).
float froxelIntensity(GpuLight l, float3 toPoint)
{
    const uint t = lightType(l);
    if (t == LIGHT_POINT) return l.intensity;
    if (t == LIGHT_SPOT)
    {
        const float s = saturate(dot(toPoint, l.forward) * l.spotScale + l.spotOffset);
        return l.intensity * s * s;
    }
    if (t == LIGHT_RECT) return l.intensity * l.size.x * l.size.y * saturate(dot(toPoint, l.forward));
    if (t == LIGHT_DISK) return l.intensity * 3.14159265 * l.size.x * l.size.x * saturate(dot(toPoint, l.forward));
    if (t == LIGHT_SPHERE) return l.intensity * 3.14159265 * l.size.x * l.size.x;
    return l.intensity * (2 * l.size.y * l.size.x + 3.14159265 * l.size.y * l.size.y);  // tube: capsule silhouette
}

// Lights the froxel lists take: the scene's g_lightCount, then the FX particle lights at the buffer's tail (A3, INTERFACES
// v1.79: StructuredBuffer<uint> g_fxLightCount [0] = F, at most g_fxLightCapacity; kNone: none). FX lights have no shadow
// slot. List entries are 16 bits with bit 15 the shadow flag, so every index is below 0x8000 (core: N + F_max <= 32,768).
uint froxelLightTotal()
{
    uint n = g_lightCount;
    if (g_fxLightCount != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> fx = ResourceDescriptorHeap[g_fxLightCount];
        n += min(fx[0], g_fxLightCapacity);
    }
    return min(n, 0x8000u);
}

#endif
