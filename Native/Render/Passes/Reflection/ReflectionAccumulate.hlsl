// unx-kernel: cs_6_6 main
// Reflection time integration (ARCHITECTURE 2.6: G path "4 rays per sample + time accumulation"; M path only where
// identity, motion and revision are proven; error table: the residual variance of the G samples is absorbed over time).
// One thread per pixel of the main view, after ReflectionResolve. For every M or G pixel with a value (a = 1):
//   identity: the pixel's scene instance (V's vis id -> visible cluster -> instance) must equal the one stored at the
//             reprojected position (per bilinear tap);
//   motion:   the pixel's surface point one frame ago is exact: the barycentric point of its triangle's previous-tick
//             vertices (Deformation.hlsli deformVertex: rigid transforms, skinning and wind alike), projected with the
//             previous view; each tap's stored linear depth must lie on that point's plane within the depth change one
//             pixel of the surface can show (1e-3 + 2 x pixel angle / |n.v|);
//   content:  the reflected content may move: the value's hit motion m (displacement per tick over the ray footprint,
//             ReflectionShade reflHitMotion; the largest of its samples) limits the window to n x m <= lobe shift;
//   revision: a scene upload, material change or history discontinuity resets every pixel (P[3].w bit 0);
//   view:     the lobe integral depends on the view direction; the history window n is limited so the reflected
//             direction's travel over it stays within reflection.temporal_lobe_shift of the lobe half-angle
//             (n <= shift x lobe / |dr| per frame; a static camera and static surface keep the full window). A mirror
//             pixel therefore accumulates only while the view is still.
// The value becomes the running mean over at most reflection.temporal_history_max values (weight 1 / (n + 1)), stored
// with n for the next frame; other pixels (K, planar, no value) store no history.
// P[0] = { reflection UAV, modes SRV, depth SRV, gbuffer SRV }
// P[1] = { vis id SRV, visible clusters SRV, distance history UAV, previous accumulation UAV (RGBA16F: mean, n) }
// P[2] = { previous keys UAV (RG32: instance + 1, linear depth), accumulation out UAV, keys out UAV, historyMax }
// P[3] = { width, height, asuint(lobe shift), flags (bit 0 reset, bit 1 off, bit 2 diagnostics: rgb = n / 32, min(motion, 1), state) }
// P[4] = { asuint(previous camera position xyz), asuint(pixel angle) }; frame constants b1 = main view.
#define UNX_CLUSTER_STREAM 1  // (the triangle's vertices from its cluster's stream when it is compressed: ClusterStream.hlsli)
#include "Passes/Reflection/ReflectionInternal.hlsli"
#include "Passes/Reflection/Reflection.hlsli"
#include "Passes/Common/VisBuffer.hlsli"
#include "Passes/Common/Deformation.hlsli"
#include "RayTracing/HalfNearest.hlsli"

// Minimal rotation taking unit a to unit b, applied to v (Rodrigues; a = -b keeps v).
float3 rotateBetween(float3 a, float3 b, float3 v)
{
    const float3 k = cross(a, b);
    const float c = dot(a, b);
    if (c < -0.9999) return v;
    return v * c + cross(k, v) + k * (dot(k, v) / (1 + c));
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 size = P[3].xy;
    if (any(pixel >= size)) return;
    RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> accumOut = ResourceDescriptorHeap[P[2].y];
    RWTexture2D<uint2> keysOut = ResourceDescriptorHeap[P[2].z];
    const float4 current = reflection[pixel];
    // A tile ReflectionClassify left all K (its validity texel, rows below the view, is 0: ReflectionBegin) has no mode
    // or value written this frame: no history (its modes and values are not this frame's).
    if (reflection[uint2(pixel.x / 8, size.y + pixel.y / 8)].a < 0.5)
    {
        keysOut[pixel] = uint2(0, 0);
        accumOut[pixel] = float4(current.rgb, 0);
        return;
    }
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].y];
    const uint mode = reflMode(modes.Load(int3(pixel, 0)));
    Texture2D<uint> visIds = ResourceDescriptorHeap[P[1].x];
    const uint visId = visIds.Load(int3(pixel, 0));
    if (current.a < 0.5 || (mode != REFL_M && mode != REFL_G) || visId == VIS_NONE)
    {
        keysOut[pixel] = uint2(0, 0);
        accumOut[pixel] = float4(current.rgb, 0);
        return;
    }
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    const GpuVisibleCluster vc = loadVisibleCluster(P[1].y, visVisibleCluster(visId));
    const uint instance = vc.instance;
    const uint key = instance + 1;
    keysOut[pixel] = uint2(key, asuint(s.linearDepth));
    RWTexture2D<float2> distanceHistory = ResourceDescriptorHeap[P[1].z];
    const float motion = distanceHistory[pixel].y;  // ReflectionResolve: the value's hit motion
    const uint flags = P[3].w;
    uint n = 0;
    float3 mean = current.rgb;
    float state = 0;  // diagnostics (flags bit 2): 0 no valid tap, 0.25 behind the camera, 0.5 taps valid with no history, 0.75 window 0, 1 integrated
    if ((flags & 3u) == 0)
    {
        // The pixel's point one frame ago: barycentric in its triangle, previous-tick vertices. An instance that did not
        // move or deform (deformInstanceStill) keeps the point: no triangle or vertex loads (most pixels of a frame).
        float3 prevPosition = s.position;
        float3 prevNormal = s.normal;
        if (!deformInstanceStillAt(instance))
        {
            const GpuInstance inst = loadInstance(instance);
            const GpuMesh mesh = loadMesh(inst.mesh);
#if UNX_CLUSTER_STREAM
            DeformedVertex d0, d1, d2;
            deformClusterTriangle(inst, mesh, loadCluster(vc.cluster), vc.cluster, visTriangle(visId), d0, d1, d2);
#else
            const uint3 tri = loadClusterTriangle(loadCluster(vc.cluster), visTriangle(visId));
            const DeformedVertex d0 = deformVertex(inst, mesh, tri.x), d1 = deformVertex(inst, mesh, tri.y), d2 = deformVertex(inst, mesh, tri.z);
#endif
            const float3 e1 = d1.world - d0.world, e2 = d2.world - d0.world, q = s.position - d0.world;
            const float3 ng = cross(e1, e2);
            const float area2 = dot(ng, ng);
            if (area2 > 1e-20)
            {
                const float b1 = dot(cross(q, e2), ng) / area2, b2 = dot(cross(e1, q), ng) / area2;
                prevPosition = d0.prevWorld + (d1.prevWorld - d0.prevWorld) * b1 + (d2.prevWorld - d0.prevWorld) * b2;
                const float3 ngPrev = cross(d1.prevWorld - d0.prevWorld, d2.prevWorld - d0.prevWorld);
                if (dot(ngPrev, ngPrev) > 1e-20) prevNormal = normalize(rotateBetween(normalize(ng), normalize(ngPrev), s.normal));
            }
        }
        const float4 clip = mul(g_prevViewProj, float4(prevPosition, 1));
        state = 0.25;
        if (clip.w > 0)
        {
            state = 0;
            const float2 ndc = clip.xy / clip.w;
            const float2 prevPixel = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * float2(size) - 0.5;
            const float3 prevCamera = asfloat(P[4].xyz);
            const float pixelAngle = asfloat(P[4].w);
            const float nv = max(abs(dot(s.normal, s.view)), 0.1);
            const float tolerance = clip.w * (1e-3 + 2 * pixelAngle / nv);
            RWTexture2D<float4> accumPrev = ResourceDescriptorHeap[P[1].w];
            RWTexture2D<uint2> keysPrev = ResourceDescriptorHeap[P[2].x];
            const int2 i0 = int2(floor(prevPixel));
            const float2 f = prevPixel - floor(prevPixel);
            float3 sum = 0;
            float weight = 0;
            uint nPrev = 0xFFFFFFFFu;
            [unroll] for (uint k = 0; k < 4; ++k)
            {
                const int2 o = int2(k & 1, k >> 1);
                const int2 t = i0 + o;
                if (any(t < 0) || any(t >= int2(size))) continue;
                const uint2 stored = keysPrev[t];
                const float4 a = accumPrev[t];  // independent of the key/depth acceptance arithmetic
                if (stored.x != key || abs(asfloat(stored.y) - clip.w) > tolerance) continue;
                const float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
                if (w <= 0) continue;
                sum += w * a.rgb;
                weight += w;
                nPrev = min(nPrev, (uint)a.a);
            }
            if (weight > 0) state = 0.5;
            if (weight > 0 && nPrev != 0xFFFFFFFFu && nPrev > 0)
            {
                state = 0.75;
                // View: the reflected direction's change since last frame against the lobe half-angle.
                const float3 r = reflect(-s.view, s.normal);
                const float3 vPrev = normalize(prevCamera - prevPosition);
                const float3 rPrev = reflect(-vPrev, prevNormal);
                const float shift = 2 * asin(saturate(0.5 * length(r - rPrev)));  // exact for small angles (acos(dot) is not)
                const float lobe = reflectionLobeHalfAngle(s.roughness, max(dot(s.normal, s.view), 1e-4));
                const float viewWindow = shift > 0 ? asfloat(P[3].z) * lobe / shift : 1e9;
                const float motionWindow = motion > 0 ? asfloat(P[3].z) / motion : 1e9;
                const float windowMax = min(viewWindow, motionWindow);
                n = (uint)min((float)min(nPrev, P[2].w), floor(windowMax));
                if (n > 0)
                {
                    mean = lerp(sum / weight, current.rgb, 1.0 / (n + 1));
                    state = 1;
                }
            }
        }
    }
    // A non-finite value (from a bad hit: never expected) neither reaches M (a = 0: M's K path there) nor enters the history,
    // where it would stay.
    if (any(isnan(mean)) || any(isinf(mean)))
    {
        reflection[pixel] = float4(0, 0, 0, 0);
        accumOut[pixel] = float4(0, 0, 0, 0);
        return;
    }
    mean = reflStorable(mean);  // finite but above RGBA16F's range would store +inf
    reflection[pixel] = (flags & 4u) ? float4(n / 32.0, min(motion, 1.0), state, current.a) : float4(mean, current.a);
    // Nearest-even before the RGBA16F store: this GPU truncates toward zero, a -2.4e-4 bias per store that the running mean
    // (weight down to 1 / historyMax) would amplify to about -2.4e-4 x historyMax in the steady state (RayTracing/HalfNearest.hlsli).
    accumOut[pixel] = float4(nearestHalf(mean), (float)min(n + 1, P[2].w));
}
