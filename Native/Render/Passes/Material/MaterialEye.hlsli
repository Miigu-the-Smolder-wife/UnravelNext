// The eye in the material resolve (MaterialModel.hlsli "Eye"; MATERIAL_EYE: a Subsurface material with an iris). Owner: M.
// The mesh is one sphere-like surface whose uv (0.5, 0.5) is where the optical axis leaves the eye; the material gives the
// axis in the mesh's object space (GpuMaterialEye::axis). Per pixel inside the iris radius:
//   frame    the axis in world space through the pixel's triangle - its rest edges and normal against its deformed ones,
//            so a rigid instance, a skinned eye and a morphed one all turn the axis as they turn the surface (no tangents
//            needed) - and the iris plane's uv directions: the triangle's dP/du and dP/dv with their part along the axis
//            removed (exact for a uv that is a projection along the axis, whatever the cornea's shape);
//   point    the view ray refracted at the shading normal into the aqueous humour meets the iris plane under the
//            cornea's cap (modelEyePoint): the base colour's uv, the mask, the limbal ring, the caustic weight.
// Outside it (the sclera) the point is the surface's own, with the outer half of the limbal ring.
// The resolve reads the base colour at MEye::uv, multiplies the ring and writes MEye::word into the class word texture
// (the anisotropy word's: one R32_UINT per pixel, there only in scenes with such materials); the material word is the
// plain one.
#ifndef UNX_M_MATERIAL_EYE_HLSLI
#define UNX_M_MATERIAL_EYE_HLSLI
#include "Passes/Material/MaterialSurface.hlsli"
#include "MaterialModel.hlsli"

struct MEye
{
    float2 uv;        // where the base colour is read
    float darkening;  // the base colour's factor
    uint word;        // modelEyePack: the iris plane's normal, the mask, the caustic weight
};

// n: the pixel's shading normal (on the viewer's side), s: its surface.
MEye mEyeEvaluate(uint visId, uint visibleClustersSrv, MSurface s, GpuMaterial m, float3 n)
{
    const GpuMaterialEye e = loadMaterialEye(m.classFlags >> 16);
    float3 axis = n, t = float3(0, 0, -1);  // (the sclera: no frame; the mask is 0 there)
    if (length(s.uv - 0.5) < e.irisRadius)
    {
        const GpuVisibleCluster vc = loadVisibleCluster(visibleClustersSrv, visVisibleCluster(visId));
        const GpuInstance inst = loadInstance(vc.instance);
        const GpuMesh mesh = loadMesh(inst.mesh);
        const uint3 tri = loadClusterTriangle(loadCluster(vc.cluster), visTriangle(visId));
        const VertexData r0 = loadVertex(mesh, tri.x), r1 = loadVertex(mesh, tri.y), r2 = loadVertex(mesh, tri.z);
        const float3 w0 = deformVertex(inst, mesh, tri.x).world;
        const float3 b1 = deformVertex(inst, mesh, tri.y).world - w0, b2 = deformVertex(inst, mesh, tri.z).world - w0;
        const float3 a1 = r1.position - r0.position, a2 = r2.position - r0.position;
        const float3 na = cross(a1, a2), nb = cross(b1, b2);
        // the axis in the rest triangle's basis (a1, a2, na), carried to the deformed one: b1, b2, and the normal by the
        // edges' scale (|nb| / |na| is the area's, its root the length's)
        const float invA = 1 / max(dot(na, na), 1e-30);
        axis = normalize(b1 * (dot(cross(a2, na), e.axis) * invA) + b2 * (dot(cross(na, a1), e.axis) * invA) +
                         nb * (dot(na, e.axis) * invA * sqrt(sqrt(dot(na, na) / max(dot(nb, nb), 1e-30)))));
        // the iris plane's uv directions (dP/du, dP/dv x the uv determinant squared: their directions are what counts)
        const float2 d1 = r1.uv - r0.uv, d2 = r2.uv - r0.uv;
        const float det = d1.x * d2.y - d1.y * d2.x;
        float3 eu = (b1 * d2.y - b2 * d1.y) * det, ev = (b2 * d1.x - b1 * d2.x) * det;
        eu -= axis * dot(axis, eu);
        ev -= axis * dot(axis, ev);
        if (!(dot(eu, eu) > 0)) eu = abs(axis.x) < 0.9 ? cross(axis, float3(1, 0, 0)) : cross(axis, float3(0, 1, 0));  // (a degenerate uv: any direction)
        eu = normalize(eu);
        if (!(dot(ev, ev) > 0)) ev = cross(axis, eu);
        ev = normalize(ev);
        const float3 r = refract(-s.view, n, 1 / e.eta);
        t = float3(dot(r, eu), dot(r, ev), dot(r, axis));
    }
    const ModelEyePoint p = modelEyePoint(e, s.uv, t);
    MEye o;
    o.uv = p.uv;
    o.darkening = p.darkening;
    o.word = modelEyePack(axis, p.mask, p.caustic);
    return o;
}

#endif
