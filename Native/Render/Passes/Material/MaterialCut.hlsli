// M's evaluation of the Cut material class (A11 join; render C's definition in CutFace.hlsli, INTERFACES 8.1 v1.71): the
// texture values of a cut face come from three object-space projections instead of uv0, with M's footprint filtering
// (anisotropic SampleGrad per projection, LEAN slope moments blended by the projection weights), then the edge damage
// band. Object space is the mesh's undeformed space: the pixel's barycentrics over the three source vertices (the
// texture stays on the fragment as it moves or deforms). The shading normal goes to world space through the triangle's
// own object -> world map (the dual basis of its world edges and normal: exact for rigid instances and per triangle for
// deformed ones).
#ifndef UNX_M_MATERIAL_CUT_HLSLI
#define UNX_M_MATERIAL_CUT_HLSLI

#include "Passes/Material/CutFace.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"

struct MCutFrame
{
    float3 p, dpdx, dpdy;   // object-space position and its screen derivatives (per pixel)
    float3 geometricNormal; // unit object-space normal of the triangle (its counter-clockwise side)
    float3 normal;          // unit interpolated object-space vertex normal
    float3 p0, p1, p2;      // object-space corners
    uint edges;             // the triangle's cut polygon boundary edges (CUT_EDGE_*)
    float footprint;        // object metres per pixel (the larger screen derivative)
    float3 oe1, oe2, we1, we2, wn;  // object and world edges, unit world geometric normal (the normal's object -> world map)
};

MCutFrame mCutFrame(uint visId, uint visibleClustersSrv, MSurface s)
{
    const GpuVisibleCluster vc = loadVisibleCluster(visibleClustersSrv, visVisibleCluster(visId));
    const GpuInstance inst = loadInstance(vc.instance);
    const GpuCluster c = loadCluster(vc.cluster);
    const GpuMesh mesh = loadMesh(inst.mesh);
    StructuredBuffer<uint> words = ResourceDescriptorHeap[g_clusterTriangles];
#if UNX_CLUSTER_STREAM
    const ClusterCorners k = loadClusterCorners(mesh, c, vc.cluster, visTriangle(visId));
    const VertexData v0 = k.v[0], v1 = k.v[1], v2 = k.v[2];
#else
    const uint3 tri = loadClusterTriangle(c, visTriangle(visId));
    const VertexData v0 = loadVertex(mesh, tri.x), v1 = loadVertex(mesh, tri.y), v2 = loadVertex(mesh, tri.z);
#endif
    const MVertex w0 = mTriangleVertex(visId, visibleClustersSrv, 0), w1 = mTriangleVertex(visId, visibleClustersSrv, 1),
                  w2 = mTriangleVertex(visId, visibleClustersSrv, 2);
    MCutFrame f;
    f.p0 = v0.position, f.p1 = v1.position, f.p2 = v2.position;
    f.p = s.bary.x * f.p0 + s.bary.y * f.p1 + s.bary.z * f.p2;
    f.dpdx = s.baryDx.x * f.p0 + s.baryDx.y * f.p1 + s.baryDx.z * f.p2;
    f.dpdy = s.baryDy.x * f.p0 + s.baryDy.y * f.p1 + s.baryDy.z * f.p2;
    f.oe1 = f.p1 - f.p0, f.oe2 = f.p2 - f.p0;
    f.geometricNormal = normalize(cross(f.oe1, f.oe2));
    f.normal = normalize(s.bary.x * v0.normal + s.bary.y * v1.normal + s.bary.z * v2.normal);
    f.edges = (words[c.triangleOffset + visTriangle(visId)] >> CUT_EDGE_SHIFT) & CUT_EDGE_MASK;
    f.footprint = max(length(f.dpdx), length(f.dpdy));
    f.we1 = w1.world - w0.world, f.we2 = w2.world - w0.world;
    f.wn = normalize(cross(f.we1, f.we2));
    return f;
}

// World direction of object-space normal n: x with x.we1 = n.oe1, x.we2 = n.oe2, x.wn = n.on (the dual basis).
float3 mCutNormalToWorld(MCutFrame f, float3 n)
{
    const float3 a = float3(dot(f.oe1, n), dot(f.oe2, n), dot(f.geometricNormal, n));
    const float3 c1 = cross(f.we2, f.wn), c2 = cross(f.wn, f.we1), c3 = cross(f.we1, f.we2);
    return normalize(a.x * c1 + a.y * c2 + a.z * c3);  // (the common factor 1 / det drops out)
}

struct MCutMaterial
{
    float3 baseColor;
    float roughness, metallic;
    float3 normal;     // world, unit (before the side and view rules)
    float variance;    // slope variance of the textures over the footprint (the geometric term is the caller's)
};

// The Cut class's material at the pixel: sum over the projections of weight x filtered tap (base colour, roughness and
// metallic factors), whiteout normals blended by weight, slope variances blended by weight; then the damage band.
// experiment: material.experiment_disable bits (1: textures, 2: normal map), as the resolve.
MCutMaterial mCutEvaluate(MCutFrame f, MSurface s, GpuMaterial m, MTextureSet ts, uint experiment)
{
    MCutMaterial o;
    float3 base = 0, normalSum = 0;
    float2 rm = 0;
    float variance = 0;
    const bool textures = (experiment & 1) == 0, normalMap = ts.moments != UNX_NONE && (experiment & 2) == 0;
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        const CutFaceProjection pr = cutFaceProjection(k, f.p, f.dpdx, f.dpdy, f.geometricNormal, m.cutScale);
        if (!(pr.weight > 0)) continue;  // (exactly 0: an axis the face is parallel to)
        float3 b = 1;
        if (ts.baseColor != UNX_NONE && textures)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.baseColor];
            b = mSampleGrad(t, (ts.flags & M_TEX_BASE_COLOR) != 0, pr.uv, pr.duvdx, pr.duvdy).rgb;
        }
        base += pr.weight * b;
        float2 r = 1;
        if (ts.roughMetal != UNX_NONE && textures)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.roughMetal];
            r = mSampleGrad(t, (ts.flags & M_TEX_ROUGH_METAL) != 0, pr.uv, pr.duvdx, pr.duvdy).xy;
        }
        rm += pr.weight * r;
        float3 tn = float3(0, 0, 1);
        if (normalMap)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[ts.moments];
            const MSlopeMoments mm = mNormalMoments(t, pr.uv, pr.duvdx, pr.duvdy, ts.slopeRange, (ts.flags & M_TEX_NORMAL) != 0);
            tn = normalize(float3(mm.mean, 1));
            variance += pr.weight * mm.variance;
        }
        normalSum += pr.weight * cutFaceWhiteout(tn, pr, f.normal);
    }
    o.baseColor = m.baseColor * base;
    o.roughness = m.roughness * rm.x;
    o.metallic = m.metallic * rm.y;
    // without a normal map the whiteout blend is exactly the interpolated normal: keep the surface's own world normal
    o.normal = normalMap ? mCutNormalToWorld(f, normalize(normalSum)) : normalize(s.normal);
    o.variance = variance;
    const float damage = cutFaceDamage(f.p, cutFaceEdgeDistance(s.bary, f.p0, f.p1, f.p2, f.edges), m.cutDamageWidth, f.footprint);
    cutFaceApplyDamage(damage, o.baseColor, o.roughness);
    return o;
}

#endif
