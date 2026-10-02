// The eye in the material resolve (MaterialModel.hlsli "Eye"; MATERIAL_EYE: a Subsurface material with an iris). Owner: M.
// The mesh is one sphere-like surface whose uv (0.5, 0.5) is where the optical axis leaves the eye; the material gives the
// axis in the mesh's object space (GpuMaterialEye::axis). Per pixel inside the iris radius:
//   frame    the axis in world space and the iris plane's uv directions through the pixel's triangle (modelEyeFrame);
//   point    the view ray refracted at the shading normal into the aqueous humour meets the iris plane under the
//            cornea's cap (modelEyePoint): the base colour's uv, the mask, the limbal ring, the caustic weight.
// Outside it (the sclera) the point is the surface's own, with the outer half of the limbal ring.
// The resolve reads the base colour at MEye::uv, multiplies the ring and writes MEye::word into the class word texture
// (the anisotropy word's: one R32_UINT per pixel, there only in scenes with such materials); the material word is the
// plain one. A coverage fragment (CoverageShade.hlsli) takes the uv and the ring alone: it has no class word, so it is
// shaded as the plain Subsurface model.
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

// n: the pixel's shading normal (on the viewer's side), s: its surface, v0..v2: its triangle's deformed vertices (the
// ones the surface was made from).
MEye mEyeEvaluate(uint visId, uint visibleClustersSrv, MSurface s, GpuMaterial m, float3 n, MVertex v0, MVertex v1, MVertex v2)
{
    const GpuMaterialEye e = loadMaterialEye(m.classFlags >> 16);
    float3 axis = n, t = float3(0, 0, -1);  // (the sclera: no frame; the mask is 0 there)
    if (length(s.uv - 0.5) < e.irisRadius)
    {
        float3 r0, r1, r2;
        mTriangleRest(visId, visibleClustersSrv, r0, r1, r2);
        const ModelEyeFrame f = modelEyeFrame(e.axis, r1 - r0, r2 - r0, v1.world - v0.world, v2.world - v0.world, v1.uv - v0.uv, v2.uv - v0.uv);
        axis = f.axis;
        t = modelEyeRay(f, -s.view, n, e.eta);
    }
    const ModelEyePoint p = modelEyePoint(e, s.uv, t);
    MEye o;
    o.uv = p.uv;
    o.darkening = p.darkening;
    o.word = modelEyePack(axis, p.mask, p.caustic);
    return o;
}

#endif
