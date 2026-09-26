// unx-kernel: cs_6_6 main
// Water view grid, pass 3 (ViewGrid.hlsli, FEATURES_GAME 1.8 B): one thread per pixel turns the scatter's winning key
// (the nearest displaced triangle over the pixel centre) into the pixel's water surface point. The mesh hit (view depth,
// perspective-correct rest position x0; the triangle's vertices evaluated again, bit-identical to the scatter's) is
// polished onto the continuous surface S_px (the same band limit as the vertices) along the pixel's ray: Newton steps on
//   x0 + D_xz(x0) = o_xz + t v_xz,  water level + h(x0) = o_y + t v_y     (unknowns x0, t; Jacobian from the slopes)
// kept when the residual is within 0.02 of the cell's across footprint and x0 stays within two cell diagonals of the
// mesh's x0 (the same sheet); otherwise the mesh point is kept.
// Outputs: surface RGBA32F (x0.x, x0.z, view depth, flags: 0 no water, 1 polished, 2 mesh point), depth R32F (view
// depth, +inf where no water).
// P[0] params SRV, 0, key SRV (raw), displacement SRV; P[1] slopes SRV, surface UAV, depth UAV, 0
#include "ViewGrid.hlsli"

#define VG_POLISH 2

void vgSample(ViewGridParams p, float2 x0, float footprint, out float4 d, out float4 s)
{
    Texture2DArray<float4> field = ResourceDescriptorHeap[P[0].w];
    Texture2DArray<float4> slopes = ResourceDescriptorHeap[P[1].x];
    d = 0;
    s = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float L = p.lengths[c];
        const float3 uv = float3(x0 / L + 0.5 / 512.0, c);
        const float mip = clamp(log2(footprint * 512.0 / L), 0.0, 9.0);
        d += field.SampleLevel(g_linearWrap, uv, mip);
        s += slopes.SampleLevel(g_linearWrap, uv, mip);
    }
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const ViewGridParams p = viewGridParams(P[0].x);
    if (pixel.x >= p.width || pixel.y >= p.height) return;
    RWTexture2D<float4> surface = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float> depthOut = ResourceDescriptorHeap[P[1].z];
    ByteAddressBuffer keys = ResourceDescriptorHeap[P[0].z];
    const uint2 key = keys.Load2((pixel.y * p.width + pixel.x) * 8);  // .x = triangle id (low), .y = depth bits (high)
    if (key.y == 0xFFFFFFFFu)
    {
        surface[pixel] = float4(0, 0, asfloat(0x7F800000u), 0);
        depthOut[pixel] = asfloat(0x7F800000u);
        return;
    }
    // The triangle's grid points (tri 0: 00 10 11, tri 1: 00 11 01).
    const uint quad = key.x >> 1;
    const int2 q = int2(quad % p.columns, quad / p.columns);
    const int2 corner[3] = { q, (key.x & 1) ? q + int2(1, 1) : q + int2(1, 0), (key.x & 1) ? q + int2(0, 1) : q + int2(1, 1) };
    float2 xy[3], x0v[3];
    float iz[3];
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        int2 fixedXy;
        uint flag;
        viewGridVertex(p, corner[k], P[0].w, fixedXy, iz[k], flag, x0v[k]);
        xy[k] = float2(fixedXy) / 256.0;
    }
    // Screen barycentrics of the pixel centre, then perspective-correct weights.
    const float2 c = float2(pixel) + 0.5;
    const float area = (xy[1].x - xy[0].x) * (xy[2].y - xy[0].y) - (xy[1].y - xy[0].y) * (xy[2].x - xy[0].x);
    float3 l = float3(((xy[1].x - c.x) * (xy[2].y - c.y) - (xy[1].y - c.y) * (xy[2].x - c.x)) / area,
                      ((xy[2].x - c.x) * (xy[0].y - c.y) - (xy[2].y - c.y) * (xy[0].x - c.x)) / area, 0);
    l.z = 1 - l.x - l.y;
    const float3 lz = l * float3(iz[0], iz[1], iz[2]);
    const float inverseDepth = lz.x + lz.y + lz.z;
    const float3 w = lz / inverseDepth;
    const float2 meshX0 = w.x * x0v[0] + w.y * x0v[1] + w.z * x0v[2];
    const float meshDepth = 1.0 / inverseDepth;
    // The pixel's ray (the projection's inverse).
    const float sx = c.x / float(p.width) * 2 - 1, sy = 1 - c.y / float(p.height) * 2;
    const float3 ray = normalize(p.forward + sx * p.tanX * p.right + sy * p.tanY * p.up);
    const float along = dot(ray, p.forward);
    float t = meshDepth / along;
    float2 x0 = meshX0;
    const float r = distance(meshX0, p.camera.xz);
    const float2 footprint = float2(r * p.theta, viewGridRow(p, uint(q.y)).y);
    const float filter = max(footprint.x, footprint.y);  // the vertices' band limit
    float4 d, s;
    [unroll] for (uint n = 0; n < VG_POLISH; ++n)
    {
        vgSample(p, x0, filter, d, s);
        const float3 f = float3(x0.x + d.x - (p.camera.x + t * ray.x), x0.y + d.z - (p.camera.z + t * ray.z), p.waterLevel + d.y - (p.camera.y + t * ray.y));
        // J = [[1 + dDx/dx, dDx/dz, -v.x], [dDx/dz, 1 + dDz/dz, -v.z], [dh/dx, dh/dz, -v.y]]; solve J delta = -f (Cramer).
        const float3 c0 = float3(1 + s.z, d.w, s.x), c1 = float3(d.w, 1 + s.w, s.y), c2 = -float3(ray.x, ray.z, ray.y);
        const float det = dot(c0, cross(c1, c2));
        if (abs(det) < 1e-8) break;
        const float3 delta = float3(dot(-f, cross(c1, c2)), dot(c0, cross(-f, c2)), dot(c0, cross(c1, -f))) / det;
        x0 += delta.xy;
        t += delta.z;
    }
    vgSample(p, x0, filter, d, s);
    const float3 residual = float3(x0.x + d.x - (p.camera.x + t * ray.x), x0.y + d.z - (p.camera.z + t * ray.z), p.waterLevel + d.y - (p.camera.y + t * ray.y));
    const bool polished = length(residual) <= 0.02 * footprint.x && distance(x0, meshX0) <= 2 * length(footprint) && t > 0;
    const float depth = polished ? t * along : meshDepth;
    surface[pixel] = polished ? float4(x0, depth, 1) : float4(meshX0, depth, 2);
    depthOut[pixel] = depth;
}
