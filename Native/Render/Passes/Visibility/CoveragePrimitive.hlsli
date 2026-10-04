// V internal: the coverage primitive of one cluster triangle, as the coverage mesh kernel (CoverageRaster.ms) and the
// compute rasteriser (CoverageRasterSw.hlsl) build it from the same deformed vertices. Owner: V.
// A primitive carries what the fragment evaluation needs inside every pixel it touches: the vis id, the material and
// flags, and the triangle clipped to the near plane (z <= w, reversed Z) as a polygon of 3 or 4 vertices in viewport
// pixels with device depth z/w and 1/w, plus their uvs (perspective-correct alpha tests) and vertex normals (world,
// octahedral 16 + 16 bits, so the record's 8 + 8 bits are the only coarse rounding). Culled: nothing left after the
// near clip, zero area, back faces of one-sided materials in views that cull back faces (the front sign follows the
// view's mirroring), the other band's triangles of a mixed sheet cluster, triangles a terrain patch replaces; and with
// visibility.coverage_triangle_cull triangles that can leave no fragment: outside the view, behind band A over the
// triangle's rectangle, or (depth buckets) behind the cover - of each pixel of a small rectangle, of the tiles of a
// larger one (CoverageBuckets.hlsli) - each a test of the whole triangle for what the fragment kernel drops fragment by
// fragment.
#ifndef UNX_COVERAGE_PRIMITIVE_HLSLI
#define UNX_COVERAGE_PRIMITIVE_HLSLI
#include "Passes/Visibility/CoverageBuckets.hlsli"
#include "VisBuffer.hlsli"

#define COV_CULL_NONE 0u
#define COV_CULL_OTHER 1u   // clip, zero area, back face, band, patch, outside the view
#define COV_CULL_BAND_A 2u  // behind band A over the triangle's rectangle
#define COV_CULL_COVER 3u   // behind the tiles' cover

// Compute rasteriser: a triangle is its work when the pixels of its rectangle inside the view are at most this many
// (the kernel's loop bound per triangle) and the near plane does not cut it.
#define COV_SW_PIXELS 8u

struct CoverageDraw  // what every triangle of one list entry shares (uniform over the group)
{
    CullView v;
    GpuInstance inst;
    GpuMesh mesh;
    GpuCluster cl;
    uint visibleIndex, material;
    uint flags;       // COV_FLAG_ALPHA | COV_FLAG_OPAQUE | COV_FLAG_TRANSLUCENT
    uint preshade;    // COV_PRESHADE_ID for M's pre-shaded classes
    bool oneSided, mixed, translucent;
    bool bounded;     // the cluster's sphere bounds its drawn vertices (not skinned, not a view model)
};

struct CoveragePrimitive
{
    uint visId, flags, material;
    float4 a, b, c, d;  // x, y (viewport pixels), z / w, 1 / w; d = c unless COV_FLAG_QUAD
    float4 tab, tcd;
    uint4 normals;      // coverageOct32 of a, b, c, d
    bool cull;
    uint reason;        // COV_CULL_*
    float2 lo, hi;      // the polygon's rectangle (pixels); set when the polygon was projected
};

float4 coverageToScreen(float4 p, float2 viewport)
{
    const float iw = 1.0 / p.w;
    return float4((p.x * iw * 0.5 + 0.5) * viewport.x, (0.5 - p.y * iw * 0.5) * viewport.y, p.z * iw, iw);
}

CoverageDraw coverageDraw(uint listEntry, bool valid, bool translucent)
{
    CoverageDraw d;
    d.visibleIndex = listEntry & ~LIST_ENTRY_MIXED;
    d.mixed = (listEntry & LIST_ENTRY_MIXED) != 0;  // band B: this raster keeps the cluster's band B triangles
    d.translucent = translucent;
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[COV_VISIBLE];
    const uint2 entry = valid ? visible[d.visibleIndex] : uint2(0, 0);
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[COV_VIEWS];
    d.v = views[entry.y >> 24];
    d.inst = loadInstance(entry.x);
    d.mesh = loadMesh(d.inst.mesh);
    d.cl = loadCluster(entry.y & 0xFFFFFFu);
    d.material = clusterMaterial(d.inst, d.cl);
    const GpuMaterial m = loadMaterial(d.material);
    d.oneSided = (m.classFlags & MATERIAL_TWO_SIDED) == 0 && (d.v.flags & CULL_VIEW_CULL_BACK) != 0;
    const uint alphaFlag = (m.classFlags & MATERIAL_ALPHA_TESTED) != 0 ? COV_FLAG_ALPHA : 0u;
    // Opaque for the view: not a see-through class (a leaf's transmission is light, not view; an alpha-tested fragment
    // hides what its mask covers after the test).
    const uint materialClass = m.classFlags & 0xFFu;
    const uint opaqueFlag = materialClass != MATERIAL_GLASS && materialClass != MATERIAL_WATER ? COV_FLAG_OPAQUE : 0u;
    d.flags = alphaFlag | opaqueFlag | (translucent ? COV_FLAG_TRANSLUCENT : 0u);
    // v1.73: M pre-shaded classes (shaded by M before the composite, which then only reads the value): Cut, Terrain (v1.75).
    // A9: layered Standard materials (clearcoat) too
    d.preshade = materialClass == MATERIAL_CUT || materialClass == MATERIAL_TERRAIN ||
                         (materialClass == MATERIAL_STANDARD && (m.classFlags & MATERIAL_LAYERED) != 0) ? COV_PRESHADE_ID : 0u;
    d.bounded = (d.inst.flags & (INSTANCE_SKINNED | INSTANCE_VIEW_MODEL)) == 0;
    return d;
}

// The cluster of a list entry lies behind the tiles' cover: none of its triangles is drawn (uniform over the group).
bool coverageDrawBehindCover(CoverageDraw d)
{
    if (COV_TILE_HIZ == UNX_NONE || !d.bounded) return false;
    return coverageSphereBehindCover(d.v, worldSphere(d.inst, d.inst.objectToWorld, d.cl.boundsSphere));
}

// Triangle t (cluster vertex indices tri) from its vertices' clip positions, uvs, normals and world positions.
CoveragePrimitive coveragePrimitive(CoverageDraw d, uint t, uint3 tri, float4 p[3], float2 uv[3], float3 nv[3], float3 world[3])
{
    // Near clip (keep w - z >= 0): Sutherland-Hodgman against one plane leaves 3 or 4 vertices (0 when behind).
    // Clip-space interpolation is linear in the world position, so the uvs interpolate with the same parameter.
    float4 q[4];
    float2 tq[4];
    float3 nq[4];
    uint n = 0;
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        const uint j = k == 2 ? 0 : k + 1;
        const float ek = p[k].w - p[k].z, ej = p[j].w - p[j].z;
        if (ek >= 0)
        {
            q[n] = p[k];
            tq[n] = uv[k];
            nq[n] = nv[k];
            ++n;
        }
        if ((ek >= 0) != (ej >= 0))
        {
            const float s = ek / (ek - ej);
            q[n] = lerp(p[k], p[j], s);
            tq[n] = lerp(uv[k], uv[j], s);
            nq[n] = lerp(nv[k], nv[j], s);
            ++n;
        }
    }
    CoveragePrimitive o = (CoveragePrimitive)0;
    o.visId = packVisId(d.visibleIndex, t) | d.preshade;
    o.material = d.material;
    o.reason = COV_CULL_OTHER;
    bool cull = n < 3 || (d.mixed && sheetTriangleBandB(d.v, world[0], world[1], world[2]) == d.translucent) || patchDropsTriangle(d.inst, d.mesh, d.cl, tri);  // C5
    if (!cull)
    {
        if (n == 3)
        {
            q[3] = q[2];
            tq[3] = tq[2];
            nq[3] = nq[2];
        }
        o.a = coverageToScreen(q[0], d.v.viewportSize);
        o.b = coverageToScreen(q[1], d.v.viewportSize);
        o.c = coverageToScreen(q[2], d.v.viewportSize);
        o.d = coverageToScreen(q[3], d.v.viewportSize);
        o.tab = float4(tq[0], tq[1]);
        o.tcd = float4(tq[2], tq[3]);
        // Signed area of the polygon (shoelace), y-down pixels.
        const float area2 = (o.a.x * o.b.y - o.b.x * o.a.y) + (o.b.x * o.c.y - o.c.x * o.b.y) + (o.c.x * o.d.y - o.d.x * o.c.y) + (o.d.x * o.a.y - o.a.x * o.d.y);
        const bool back = COV_FRONT_SIGN * area2 > 0;
        o.flags = d.flags | (n == 4 ? COV_FLAG_QUAD : 0u) | (back ? COV_FLAG_BACK : 0u);
        cull = area2 == 0 || (d.oneSided && back);
        o.lo = min(min(o.a.xy, o.b.xy), min(o.c.xy, o.d.xy));
        o.hi = max(max(o.a.xy, o.b.xy), max(o.c.xy, o.d.xy));
        if (!cull && COV_RASTER_TRIANGLE_CULL)
        {
            // No pixel of the view holds any of it; or every fragment it can leave is one the fragment kernel drops
            // (its depth lies inside the polygon's vertex depths, CoverageFragment.hlsli).
            const float nearest = max(max(o.a.z, o.b.z), max(o.c.z, o.d.z));
            if (o.hi.x <= 0 || o.hi.y <= 0 || o.lo.x >= (float)COV_WIDTH || o.lo.y >= (float)COV_HEIGHT) cull = true;
            else if (coverageRectBehindBandA(o.lo, o.hi, nearest))
            {
                cull = true;
                o.reason = COV_CULL_BAND_A;
            }
            else if (coverageRectBehindCoverPixels(o.lo, o.hi, nearest) || coverageRectBehindCover(o.lo, o.hi, nearest, COV_TILE_SPAN_TRIANGLE))
            {
                cull = true;
                o.reason = COV_CULL_COVER;
            }
        }
        // Culled primitives never export a fragment normal; avoid four oct
        // encodes (divides/rounds) after their visibility has already been decided.
        if (!cull) o.normals = uint4(coverageOct32(nq[0]), coverageOct32(nq[1]), coverageOct32(nq[2]), coverageOct32(nq[3]));
    }
    o.cull = cull;
    if (!cull) o.reason = COV_CULL_NONE;
    return o;
}

// The compute rasteriser's triangles: not near-clipped, and at most COV_SW_PIXELS pixels of the view under the rectangle
// (first pixel and size of that pixel rectangle out).
bool coveragePrimitiveSmall(CoveragePrimitive o, out int2 first, out uint2 size)
{
    const int2 view = int2(COV_WIDTH, COV_HEIGHT);
    first = max(int2(floor(o.lo)), 0);
    const int2 last = min(int2(floor(o.hi)), view - 1);
    size = uint2(max(last - first + 1, 0));
    return !o.cull && (o.flags & COV_FLAG_QUAD) == 0 && size.x * size.y != 0 && size.x * size.y <= COV_SW_PIXELS;
}

#endif
