// unx-kernel: cs_6_6 main
// Temporal upscale (Upscale.cpp), motion per internal sample: where the surface point a jittered sample sees was in the
// previous frame's unjittered view, as an output-UV offset: m = uv_now - uv_prev, uv_now the point's unjittered position
// (the sample centre k + 0.5 minus this frame's jitter), uv_prev its previous surface point (GiScreenHistory.hlsli
// giPreviousSurface's rule: the vis buffer's triangle in its previous-tick vertices; still instances keep the point)
// projected with the previous unjittered view-projection. The sky: the pixel's direction under the previous view
// (rotation only). A point behind the previous camera: m = (2, 2) (the history lookup falls off the screen and is not
// used). RG32F (UV offsets need more than half precision at 4K: 1/3840 per pixel).
// The layers over the opaque surface (output.upscale_layer_motion; P[6].y): the sample's vector is the front-most
// surface's that the picture at the pixel is of, and the pixel's layers are named for the upscale's rejection:
//   glass      a pane or body covering the whole pixel (V's translucent layer, class 1): its triangle's own motion;
//   water      the water layer's surface: the still point at its depth (the surface's waves are not followed);
//   thin       the coverage layer's opaque fragments in front of the opaque surface, where they cover a third of the
//              pixel or more: the nearest one's own motion (a cluster fragment's triangle; a hair or stream fragment
//              moves as the surface behind it does, carried to the fragment's depth);
//   particles and the coverage layer's see-through fragments have no vector of their own: the pixel keeps the vector
//              of what lies behind them and is marked animated by their opacity.
// The tracked depth (the surface the vector is of) replaces the view's depth in m.tsr.dilate: the closest surface of a
// neighbourhood, the parallax test and the motion blur take the layer's depth where a layer has the pixel.
// P[0] = { vis id SRV (UNX_NONE: none), visible clusters SRV, depth SRV, motion UAV }, P[1] = { width, height,
// asuint(jitter x), asuint(jitter y) } (internal pixels), P[2..5] = rows of the previous unjittered view-projection.
// P[6] = { previous depth UAV (RG32F; UNX_NONE: none), tracked depth UAV (R32F device depth; UNX_NONE: no layers),
//          layers UAV (RGBA8: r = the thin fragments' coverage of the pixel, g = animated - no vector, the history is
//          clamped and short -, b = seen through - a tracked glass or water whose background moves otherwise, the
//          history is clamped -, a = 0), coverage tiles per row }
// P[7] = { translucent vis SRV, translucent depth SRV (linear view depth), translucent class SRV (UNX_NONE: no glass),
//          water depth SRV (linear view depth; UNX_NONE: none) }
// P[8] = { coverage tiles SRV (raw; UNX_NONE: no coverage layer), coverage tile pixels SRV (raw), coverage records SRV,
//          coverage depth range SRV }, P[9] = { particle layer SRV (UNX_NONE: none), particle edges SRV, 0, 0 }
// previous depth: r = the point's view depth in the previous frame (0: the sky, or
// behind the previous camera) - the temporal super resolution's parallax test (Tsr.hlsli); g = how much the point
// moves (0 still .. 1): its own displacement in the world over two pixel radii less one, or its parallax - the screen
// travel beyond the camera's rotation - over 10 px of a 1920-wide view at 60 Hz, less a half (the flickering
// heuristic keeps its history only on what stands still: the reference's IsMovingMask); an animated layer moves.
// Frame constants of the (jittered) main view.
#include "Bindless.hlsli"
#include "Passes/Common/Frame.hlsli"
#include "Passes/Common/VisBuffer.hlsli"
#include "Passes/Common/Deformation.hlsli"
#include "Passes/Visibility/CoverageTiles.hlsli"
#include "Passes/FX/ParticleLayer.hlsli"

#define LAYER_FRAGMENTS 16u      // a pixel's records read at most (more: the first ones)
#define LAYER_THIN_COVERAGE (1.0 / 3.0)

float4 prevClipOf(float4 p)
{
    const float4x4 m = float4x4(asfloat(P[2]), asfloat(P[3]), asfloat(P[4]), asfloat(P[5]));
    return mul(m, p);
}

// The previous-tick position of point p moving with the triangle of 'visId' (GiScreenHistory.hlsli giPreviousSurface's
// position: the point's barycentric coordinates and its height above the triangle, in the previous-tick vertices).
float3 previousPointOf(uint visId, uint visibleClustersSrv, float3 p)
{
    if (visId == VIS_NONE) return p;
    const GpuVisibleCluster vc = loadVisibleCluster(visibleClustersSrv, visVisibleCluster(visId));
    const GpuInstance inst = loadInstance(vc.instance);
    if (deformInstanceStill(inst)) return p;  // the point itself (no vertex loads)
    const GpuMesh mesh = loadMesh(inst.mesh);
    const uint3 tri = loadClusterTriangle(loadCluster(vc.cluster), visTriangle(visId));
    const DeformedVertex d0 = deformVertex(inst, mesh, tri.x), d1 = deformVertex(inst, mesh, tri.y), d2 = deformVertex(inst, mesh, tri.z);
    const float3 e1 = d1.world - d0.world, e2 = d2.world - d0.world, q = p - d0.world;
    const float3 ng = cross(e1, e2);
    const float area2 = dot(ng, ng);
    if (!(area2 > 1e-20)) return p;
    const float b1 = dot(cross(q, e2), ng) / area2, b2 = dot(cross(e1, q), ng) / area2;
    const float h = dot(q, ng) / sqrt(area2);
    const float3 ngPrev = cross(d1.prevWorld - d0.prevWorld, d2.prevWorld - d0.prevWorld);
    const bool prevValid = dot(ngPrev, ngPrev) > 1e-20;
    return d0.prevWorld + (d1.prevWorld - d0.prevWorld) * b1 + (d2.prevWorld - d0.prevWorld) * b2 + (prevValid ? normalize(ngPrev) * h : 0);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 size = P[1].xy;
    if (any(id >= size)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float2> motion = ResourceDescriptorHeap[P[0].w];
    const float2 jitter = asfloat(P[1].zw);
    const float2 uvNow = (float2(id) + 0.5 - jitter) / float2(size);
    const float opaque = depth.Load(int3(id, 0));
    uint visId = VIS_NONE;
    if (P[0].x != UNX_NONE)
    {
        Texture2D<uint> visIds = ResourceDescriptorHeap[P[0].x];
        visId = visIds.Load(int3(id, 0));
    }

    // the surface the vector is of
    float d = opaque;
    float thin = 0, animated = 0;
    bool seeThrough = false;  // (a glass or water surface is tracked)
    if (P[6].y != UNX_NONE)
    {
        if (P[7].z != UNX_NONE)
        {
            Texture2D<uint> translucentClass = ResourceDescriptorHeap[P[7].z];
            if (translucentClass.Load(int3(id, 0)) == 1u)
            {
                Texture2D<float> translucentDepth = ResourceDescriptorHeap[P[7].y];
                Texture2D<uint> translucentVis = ResourceDescriptorHeap[P[7].x];
                const float view = translucentDepth.Load(int3(id, 0));
                const float z = view > 0 ? g_nearPlane / view : 0.0;
                const uint glass = translucentVis.Load(int3(id, 0));
                if (z > d && glass != VIS_NONE)
                {
                    d = z;
                    visId = glass;
                    seeThrough = true;
                }
            }
        }
        if (P[7].w != UNX_NONE)
        {
            Texture2D<float> waterDepth = ResourceDescriptorHeap[P[7].w];
            const float view = waterDepth.Load(int3(id, 0));
            const float z = view > 0 ? g_nearPlane / view : 0.0;
            if (z > d)
            {
                d = z;
                visId = VIS_NONE;  // (the still point at the surface's depth)
                seeThrough = true;
            }
        }
        if (P[8].x != UNX_NONE)
        {
            Texture2D<uint2> depthRange = ResourceDescriptorHeap[P[8].w];
            const uint2 range = depthRange.Load(int3(id, 0));
            if (!(range.x == 0u && range.y == 0xFFFFFFFFu) && asfloat(range.x) > opaque)
            {
                ByteAddressBuffer tiles = ResourceDescriptorHeap[P[8].x];
                const uint tile = (id.x / COV_TILE_PX) + (id.y / COV_TILE_PX) * P[6].w;
                const uint3 header = tiles.Load3(4u * COV_TILE_WORDS * tile);  // records, record base, listed index + 1
                if (header.z != 0u)
                {
                    ByteAddressBuffer tilePixels = ResourceDescriptorHeap[P[8].y];
                    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[8].z];
                    const uint listed = header.z - 1u, pixelInTile = (id.x % COV_TILE_PX) + COV_TILE_PX * (id.y % COV_TILE_PX);
                    const uint first = tilePixels.Load(4u * (listed * COV_TILE_PIXELS + pixelInTile));
                    const uint end = pixelInTile + 1u < COV_TILE_PIXELS ? tilePixels.Load(4u * (listed * COV_TILE_PIXELS + pixelInTile + 1u)) : header.x;
                    float nearest = 0, see = 0;
                    uint nearestVis = VIS_NONE;
                    const uint count = min(end - min(first, end), LAYER_FRAGMENTS);
                    [loop] for (uint i = 0; i < count; ++i)
                    {
                        const CoverageFragment f = coverageUnpackRecord(records[header.y + first + i]);
                        const float z = coverageFragmentDepth(f);
                        if (!(z > opaque)) continue;  // (behind the opaque surface: not in the picture)
                        if (!coverageFragmentOpaque(f))
                        {
                            see += coverageFragmentArea(f);
                            continue;
                        }
                        thin += coverageFragmentArea(f);
                        if (z > nearest)
                        {
                            nearest = z;
                            // (a cluster fragment's own triangle; hair and stream fragments: the surface behind)
                            nearestVis = (f.visId >> 31) == 0u ? coverageClusterVisId(f.visId) : VIS_NONE;
                        }
                    }
                    thin = saturate(thin);
                    animated = saturate(see);
                    if (thin >= LAYER_THIN_COVERAGE && nearest > d)
                    {
                        d = nearest;
                        if (nearestVis != VIS_NONE) visId = nearestVis;
                        seeThrough = false;
                    }
                }
            }
        }
        if (P[9].x != UNX_NONE)
        {
            Texture2D<float4> particleLayer = ResourceDescriptorHeap[P[9].x];
            ByteAddressBuffer particleEdges = ResourceDescriptorHeap[P[9].y];
            animated = max(animated, saturate(1.0 - fxParticleLayerAt(particleLayer, particleEdges, id).a));
        }
    }

    float4 prevClip;
    float moving = 0, seenThrough = 0;
    if (!(d > 0))
    {
        const float3 dir = worldFromDepth(float2(id), 1e-6) - g_cameraPosition;  // a point far along the pixel's ray
        prevClip = prevClipOf(float4(dir, 0));
    }
    else
    {
        const float3 p = worldFromDepth(float2(id), d);
        const float3 prevP = previousPointOf(visId, P[0].y, p);
        prevClip = prevClipOf(float4(prevP, 1));
        // is-moving: the point's own displacement, or its parallax beyond the camera's rotation
        const float radius = linearDepth(d) * g_tanHalfFovY / g_viewHeight;
        const float4 still = prevClipOf(float4(p, 1)), turned = prevClipOf(float4(p - g_cameraPosition, 0));
        float parallax = 0;
        if (still.w > 1e-6 && turned.w > 1e-6) parallax = 0.5 * length((turned.xy / turned.w - still.xy / still.w) * float2(size));
        const float maxParallax = max(g_deltaTime * 60.0, 1e-3) * 10.0 * (float)size.x / 1920.0;
        moving = max(saturate(distance(p, prevP) / max(2.0 * radius, 1e-9) - 1.0), saturate(parallax / maxParallax - 0.5));
        if (seeThrough && prevClip.w > 1e-6)
        {
            // what lies behind a tracked glass or water surface, as a still point: where its screen travel differs from
            // the surface's by more than a pixel the picture seen through the surface moves against the history
            const float4 behind = opaque > 0 ? prevClipOf(float4(worldFromDepth(float2(id), opaque), 1)) : prevClipOf(float4(p - g_cameraPosition, 0));
            if (behind.w > 1e-6) seenThrough = saturate(0.5 * length((behind.xy / behind.w - prevClip.xy / prevClip.w) * float2(size)) - 1.0);
        }
    }
    float2 m = float2(2, 2);
    if (prevClip.w > 1e-6)
    {
        const float2 ndc = prevClip.xy / prevClip.w;
        m = uvNow - float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    }
    motion[id] = all(isfinite(m)) ? m : float2(2, 2);
    if (P[6].x != UNX_NONE)
    {
        RWTexture2D<float2> previousDepth = ResourceDescriptorHeap[P[6].x];
        previousDepth[id] = float2(d > 0 && prevClip.w > 1e-6 && isfinite(prevClip.w) ? prevClip.w : 0.0, isfinite(moving) ? max(moving, animated) : 1.0);
    }
    if (P[6].y != UNX_NONE)
    {
        RWTexture2D<float> trackedDepth = ResourceDescriptorHeap[P[6].y];
        RWTexture2D<float4> layers = ResourceDescriptorHeap[P[6].z];
        trackedDepth[id] = d;
        layers[id] = float4(thin, animated, isfinite(seenThrough) ? seenThrough : 1.0, 0);
    }
}
