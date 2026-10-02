// Surface under a pixel from the visibility buffer (ARCHITECTURE 2.2). Owner: M. Used by the material resolve and,
// later, by edge (E) and coverage-fragment shading, so every consumer reconstructs the same surface.
//
// The pixel-centre ray is intersected with the plane of the deformed triangle (the same deformVertex() V rasterised),
// and the barycentrics and their screen derivatives are analytic:
//   ray      C + t D,  D = camera-space direction of the pixel centre rotated to world (no large-coordinate differences)
//   plane    n = e1 x e2,  t = n.(P0 - C) / n.D
//   bary     r = t D - (P0 - C),  b1 = n.(r x e2) / |n|^2,  b2 = n.(e1 x r) / |n|^2
//   deriv.   dr/dx = t (Dx - D (n.Dx) / (n.D))  (the hit point's motion along the plane for a one-pixel step)
// Everything is camera-relative, so the only cancellation is r = tD - (P0 - C) at relative precision eps d / s
// (d distance, s triangle size: 1e-4 for 1 px triangles at 2 km), instead of the eps (d / s)^2 of a clip-space
// determinant.
#ifndef UNX_M_MATERIAL_SURFACE_HLSLI
#define UNX_M_MATERIAL_SURFACE_HLSLI
#include "VisBuffer.hlsli"
#include "Deformation.hlsli"

// World direction of the ray through a pixel position (pixel units, centre = +0.5) and its derivatives per pixel.
// Every projection of this renderer has clip.w = -z_view (row 3 = 0 0 -1 0) and no shear, including the cropped
// planar-reflection projections (FrameRenderer.cpp), so the view-space direction at z = -1 is
//   x = (ndc.x + P02 - P03) / P00,  y = (ndc.y + P12 - P13) / P11.
void mPixelRay(float2 pixelPos, out float3 D, out float3 Dx, out float3 Dy)
{
    const float2 ndc = float2(pixelPos.x / g_viewWidth * 2 - 1, 1 - pixelPos.y / g_viewHeight * 2);
    const float vx = (ndc.x + g_proj[0][2] - g_proj[0][3]) / g_proj[0][0];
    const float vy = (ndc.y + g_proj[1][2] - g_proj[1][3]) / g_proj[1][1];
    const float3 ax = g_view[0].xyz, ay = g_view[1].xyz, az = g_view[2].xyz;  // camera axes in world (orthonormal)
    D = ax * vx + ay * vy - az;
    Dx = ax * (2.0 / (g_viewWidth * g_proj[0][0]));
    Dy = ay * (-2.0 / (g_viewHeight * g_proj[1][1]));
}

struct MSurface
{
    uint instance, cluster, material;
    float3 offset;          // hit position relative to the camera (world axes)
    float3 dpdx, dpdy;      // its screen derivatives on the triangle's plane (per pixel)
    float3 geometricNormal; // unit normal of the triangle's plane, on its counter-clockwise (authored) side
    float3 view;            // unit, towards the camera
    bool front;             // ray meets the counter-clockwise (authored) side
    float3 normal;          // interpolated vertex normal (not normalised; MikkTSpace per-pixel convention)
    float3 dndx, dndy;      // screen derivatives of the normalised interpolated normal
    float3 tangent;         // interpolated vertex tangent (not normalised)
    float tangentSign;
    float2 uv, duvdx, duvdy;
    float3 bary, baryDx, baryDy;
};

// One deformed vertex of the triangle a vis id names (camera-relative position, world normal and tangent, uv).
struct MVertex
{
    float3 world;        // world position (edges are differences of these; the camera enters once, in r0)
    float3 normal;       // world, unit
    float3 tangent;      // world, unit
    float2 uv;
    float tangentSign;
};

struct MTriangleIdentity
{
    uint instance, cluster, material;
};

MTriangleIdentity mTriangleIdentity(uint visId, uint visibleClustersSrv)
{
    const GpuVisibleCluster vc = loadVisibleCluster(visibleClustersSrv, visVisibleCluster(visId));
    MTriangleIdentity id;
    id.instance = vc.instance;
    id.cluster = vc.cluster;
    id.material = clusterMaterial(loadInstance(vc.instance), loadCluster(vc.cluster));
    return id;
}

MVertex mTriangleVertex(uint visId, uint visibleClustersSrv, uint corner)
{
    const GpuVisibleCluster vc = loadVisibleCluster(visibleClustersSrv, visVisibleCluster(visId));
    const GpuInstance inst = loadInstance(vc.instance);
    const GpuCluster c = loadCluster(vc.cluster);
    const GpuMesh mesh = loadMesh(inst.mesh);
#if UNX_CLUSTER_STREAM  // (the cluster's own vertices when it is compressed: ClusterStream.hlsli)
    uint meshVertex;
    const VertexData v = loadClusterCorner(mesh, c, vc.cluster, visTriangle(visId), corner, meshVertex);
    const DeformedVertex d = deformLoadedVertex(inst, mesh, v, meshVertex);
#else
    const uint3 tri = loadClusterTriangle(c, visTriangle(visId));
    const uint meshVertex = corner == 0 ? tri.x : (corner == 1 ? tri.y : tri.z);
    const DeformedVertex d = deformVertex(inst, mesh, meshVertex);
    const VertexData v = loadVertex(mesh, meshVertex);
#endif
    MVertex o;
    o.world = d.world;
    o.normal = d.normal;
    o.tangent = d.tangent;
    o.uv = v.uv;
    o.tangentSign = v.tangentSign;
    return o;
}

// Rest-pose positions (object space) of the triangle a vis id names.
void mTriangleRest(uint visId, uint visibleClustersSrv, out float3 r0, out float3 r1, out float3 r2)
{
    const GpuVisibleCluster vc = loadVisibleCluster(visibleClustersSrv, visVisibleCluster(visId));
    const GpuMesh mesh = loadMesh(loadInstance(vc.instance).mesh);
#if UNX_CLUSTER_STREAM
    const ClusterCorners k = loadClusterCorners(mesh, loadCluster(vc.cluster), vc.cluster, visTriangle(visId));
    r0 = k.v[0].position;
    r1 = k.v[1].position;
    r2 = k.v[2].position;
#else
    const uint3 tri = loadClusterTriangle(loadCluster(vc.cluster), visTriangle(visId));
    r0 = loadVertex(mesh, tri.x).position;
    r1 = loadVertex(mesh, tri.y).position;
    r2 = loadVertex(mesh, tri.z).position;
#endif
}

// The triangle a vis id names as coverage needs it (records loaded once): deformed world positions, material, and the
// vertex uv (loaded only where the caller reads it).
struct MTriangleCorners
{
    float3 w0, w1, w2;
    float2 uv0, uv1, uv2;
    uint material;
};

MTriangleCorners mTriangleCorners(uint visId, uint visibleClustersSrv)
{
    const GpuVisibleCluster vc = loadVisibleCluster(visibleClustersSrv, visVisibleCluster(visId));
    const GpuInstance inst = loadInstance(vc.instance);
    const GpuCluster c = loadCluster(vc.cluster);
    const GpuMesh mesh = loadMesh(inst.mesh);
    MTriangleCorners o;
#if UNX_CLUSTER_STREAM
    const ClusterCorners k = loadClusterCorners(mesh, c, vc.cluster, visTriangle(visId));
    o.w0 = deformLoadedVertex(inst, mesh, k.v[0], k.meshVertex.x).world;
    o.w1 = deformLoadedVertex(inst, mesh, k.v[1], k.meshVertex.y).world;
    o.w2 = deformLoadedVertex(inst, mesh, k.v[2], k.meshVertex.z).world;
    o.uv0 = k.v[0].uv;
    o.uv1 = k.v[1].uv;
    o.uv2 = k.v[2].uv;
#else
    const uint3 tri = loadClusterTriangle(c, visTriangle(visId));
    o.w0 = deformVertex(inst, mesh, tri.x).world;
    o.w1 = deformVertex(inst, mesh, tri.y).world;
    o.w2 = deformVertex(inst, mesh, tri.z).world;
    o.uv0 = loadVertex(mesh, tri.x).uv;
    o.uv1 = loadVertex(mesh, tri.y).uv;
    o.uv2 = loadVertex(mesh, tri.z).uv;
#endif
    o.material = clusterMaterial(inst, c);
    return o;
}

// uv of a triangle's plane under the pixel-centre ray (D, Dx, Dy of mPixelRay) and its screen derivatives, by the
// barycentric formulas of mSurfaceFromVertices (the point may lie outside the triangle: the plane's affine uv).
void mPlaneUv(MTriangleCorners t, float3 D, float3 Dx, float3 Dy, out float2 uv, out float2 duvdx, out float2 duvdy)
{
    const float3 r0 = t.w0 - g_cameraPosition;
    const float3 e1 = t.w1 - t.w0, e2 = t.w2 - t.w0;
    const float3 n = cross(e1, e2);
    const float nD = dot(n, D);
    const float tHit = dot(n, r0) / nD;
    const float inv = 1.0 / dot(n, n);
    const float3 r = tHit * D - r0;
    const float3 rx = tHit * (Dx - D * (dot(n, Dx) / nD)), ry = tHit * (Dy - D * (dot(n, Dy) / nD));
    const float2 u1 = t.uv1 - t.uv0, u2 = t.uv2 - t.uv0;
    uv = t.uv0 + u1 * (dot(n, cross(r, e2)) * inv) + u2 * (dot(n, cross(e1, r)) * inv);
    duvdx = u1 * (dot(n, cross(rx, e2)) * inv) + u2 * (dot(n, cross(e1, rx)) * inv);
    duvdy = u1 * (dot(n, cross(ry, e2)) * inv) + u2 * (dot(n, cross(e1, ry)) * inv);
}

// The surface of a triangle (three deformed vertices) under the pixel-centre ray.
MSurface mSurfaceFromVertices(MTriangleIdentity id, MVertex v0, MVertex v1, MVertex v2, float2 pixelPos)
{
    MSurface s;
    s.instance = id.instance;
    s.cluster = id.cluster;
    s.material = id.material;

    float3 D, Dx, Dy;
    mPixelRay(pixelPos, D, Dx, Dy);
    const float3 r0 = v0.world - g_cameraPosition;
    const float3 e1 = v1.world - v0.world, e2 = v2.world - v0.world;
    const float3 n = cross(e1, e2);
    const float nD = dot(n, D);
    const float t = dot(n, r0) / nD;
    const float3 r = t * D - r0;
    const float inv = 1.0 / dot(n, n);
    const float b1 = dot(n, cross(r, e2)) * inv, b2 = dot(n, cross(e1, r)) * inv;
    const float3 rx = t * (Dx - D * (dot(n, Dx) / nD)), ry = t * (Dy - D * (dot(n, Dy) / nD));
    const float b1x = dot(n, cross(rx, e2)) * inv, b2x = dot(n, cross(e1, rx)) * inv;
    const float b1y = dot(n, cross(ry, e2)) * inv, b2y = dot(n, cross(e1, ry)) * inv;
    s.bary = float3(1 - b1 - b2, b1, b2);
    s.baryDx = float3(-b1x - b2x, b1x, b2x);
    s.baryDy = float3(-b1y - b2y, b1y, b2y);

    s.offset = t * D;
    s.dpdx = rx;
    s.dpdy = ry;
    s.geometricNormal = n * rsqrt(dot(n, n));
    s.view = -normalize(D);
    s.front = nD < 0;

    s.uv = s.bary.x * v0.uv + s.bary.y * v1.uv + s.bary.z * v2.uv;
    s.duvdx = s.baryDx.x * v0.uv + s.baryDx.y * v1.uv + s.baryDx.z * v2.uv;
    s.duvdy = s.baryDy.x * v0.uv + s.baryDy.y * v1.uv + s.baryDy.z * v2.uv;

    s.normal = s.bary.x * v0.normal + s.bary.y * v1.normal + s.bary.z * v2.normal;
    const float3 nx = s.baryDx.x * v0.normal + s.baryDx.y * v1.normal + s.baryDx.z * v2.normal;
    const float3 ny = s.baryDy.x * v0.normal + s.baryDy.y * v1.normal + s.baryDy.z * v2.normal;
    const float len = length(s.normal);
    const float3 nh = s.normal / len;
    s.dndx = (nx - nh * dot(nh, nx)) / len;
    s.dndy = (ny - nh * dot(nh, ny)) / len;

    s.tangent = s.bary.x * v0.tangent + s.bary.y * v1.tangent + s.bary.z * v2.tangent;
    s.tangentSign = v0.tangentSign;
    return s;
}

MSurface mSurfaceFromVis(uint visId, uint visibleClustersSrv, float2 pixelPos)
{
    return mSurfaceFromVertices(mTriangleIdentity(visId, visibleClustersSrv), mTriangleVertex(visId, visibleClustersSrv, 0),
                                mTriangleVertex(visId, visibleClustersSrv, 1), mTriangleVertex(visId, visibleClustersSrv, 2), pixelPos);
}

#endif
