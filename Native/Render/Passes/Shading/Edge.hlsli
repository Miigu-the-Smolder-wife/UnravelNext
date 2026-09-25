// Edge (E) pixels and their surface groups (ARCHITECTURE 2.10; INTERFACES 5.5.1). Owner: M.
//
// Two pixels see the same surface when both are sky, or both show the same material and each lies on the other's
// tangent plane (shading normal) and their normals agree:
//   |(P_q - P_p) . n_p| and |(P_p - P_q) . n_q| <= max(k_f footprint, k_d |P_q - P_p|),  n_p . n_q >= cos(angle).
// The relation is symmetric, so a pixel an edge pixel borrows radiance from is itself an edge pixel. The distance term
// grows with the pixels' separation, which keeps a bumpy (normal-mapped) surface seen at a grazing angle continuous while
// a depth gap (silhouette) exceeds it; the normal term finds hard creases. A pixel is an edge pixel (E) when a 3 x 3
// neighbour sees another surface. Triangle boundaries inside one smooth surface are not edges.
#ifndef UNX_M_EDGE_HLSLI
#define UNX_M_EDGE_HLSLI
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"

struct EdgeParams
{
    float cosAngle;           // shading.edge_normal_angle_deg
    float footprintTolerance; // shading.edge_footprint_tolerance (pixel footprints)
    float distanceTolerance;  // shading.edge_distance_tolerance (fraction of the pixels' separation)
};

struct EdgePixel
{
    bool sky;
    uint material;
    float3 position;  // camera-relative
    float3 normal;
    float footprint;  // lateral size of one pixel at the surface (m)
};

EdgePixel edgePixel(uint2 q, Texture2D<uint> words, Texture2D<float> depth, Texture2D<uint2> gbuffer)
{
    EdgePixel e;
    const uint word = words[q];
    e.material = mWordMaterial(word);
    e.sky = e.material == M_MATERIAL_SKY;
    e.position = 0;
    e.normal = 0;
    e.footprint = 0;
    if (!e.sky)
    {
        float3 D, Dx, Dy;
        mPixelRay(float2(q) + 0.5, D, Dx, Dy);
        const float z = linearDepth(depth[q]);
        e.position = D * z;
        e.normal = decodeGBuffer(gbuffer[q]).normal;
        e.footprint = length(Dx) * z;
    }
    return e;
}

bool edgeSameSurface(EdgePixel a, EdgePixel b, EdgeParams p)
{
    if (a.sky || b.sky) return a.sky && b.sky;
    if (a.material != b.material) return false;
    if (dot(a.normal, b.normal) < p.cosAngle) return false;
    const float3 d = b.position - a.position;
    const float tol = max(p.footprintTolerance * max(a.footprint, b.footprint), p.distanceTolerance * length(d));
    return abs(dot(d, a.normal)) <= tol && abs(dot(d, b.normal)) <= tol;
}

// Is 'pixel' an edge pixel: some in-view 3 x 3 neighbour sees another surface.
bool edgeIsEdge(uint2 pixel, Texture2D<uint> words, Texture2D<float> depth, Texture2D<uint2> gbuffer, EdgeParams p)
{
    const EdgePixel c = edgePixel(pixel, words, depth, gbuffer);
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        if (k == 4) continue;
        const int2 q = int2(pixel) + int2(int(k % 3) - 1, int(k / 3) - 1);
        if (any(q < 0) || q.x >= int(g_viewWidth) || q.y >= int(g_viewHeight)) continue;
        if (!edgeSameSurface(c, edgePixel(uint2(q), words, depth, gbuffer), p)) return true;
    }
    return false;
}

// Marks an edge tile once and appends it to the edge list (tile flags zeroed by the resolve).
void edgeAppendTile(uint2 tile, bool anyEdge, uint tilesX, uint flagsUav, uint listUav, uint argsUav)
{
    if (!anyEdge) return;
    RWByteAddressBuffer flags = ResourceDescriptorHeap[flagsUav];
    uint old;
    flags.InterlockedOr(4 * (tile.y * tilesX + tile.x), 1u, old);
    if (old & 1u) return;
    RWByteAddressBuffer args = ResourceDescriptorHeap[argsUav];
    RWByteAddressBuffer list = ResourceDescriptorHeap[listUav];
    uint slot;
    args.InterlockedAdd(0, 1, slot);
    list.Store(4 * slot, tile.x | (tile.y << 16));
}

#endif
