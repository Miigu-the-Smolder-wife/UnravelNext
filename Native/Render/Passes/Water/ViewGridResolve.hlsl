// unx-kernel: cs_6_6 main
// Water view grid, pass 3 (ViewGrid.hlsli, FEATURES_GAME 1.8 B): one thread per pixel turns the scatter's winning key
// (the nearest displaced triangle over the pixel centre) into the pixel's water surface point. The mesh hit (view depth,
// perspective-correct rest position x0; the triangle's vertices evaluated again, bit-identical to the scatter's) is
// polished onto the continuous surface S_px (the same band limit as the vertices) along the pixel's ray: Newton steps on
//   x0 + D_xz(x0) = o_xz + t v_xz,  water level + h(x0) = o_y + t v_y     (unknowns x0, t; Jacobian from the slopes)
// kept when the residual is within 0.05 of the pixel footprint and x0 stays within two cell diagonals of the
// mesh's x0 (the same sheet); otherwise the mesh point is kept.
// Outputs: surface RGBA32F (x0.x, x0.z, view depth, flags: 0 no water, 1 polished, 2 mesh point), depth R32F (view
// depth, +inf where no water).
// P[0] params SRV, 0, key SRV (raw), displacement SRV; P[1] slopes SRV, surface UAV, depth UAV, error UAV (R32F, 0 =
// none: the output point's distance to the surface along its normal, (S(x0) - P) . n over the pixel footprint; tests)
#include "ViewGrid.hlsli"

#define VG_POLISH 4  // Newton steps at most; a converged pixel stops (most take one or two)

// F(x0, t) = S(x0) - ray(t) with S(x0) = (x0.x + Dx, water level + h, x0.y + Dz), in (x, z, y) order, and the normal.
float3 vgResidual(ViewGridParams p, OceanPoint o, float2 x0, float t, float3 ray)
{
    return float3(x0.x + o.D.x - (p.camera.x + t * ray.x), x0.y + o.D.z - (p.camera.z + t * ray.z), p.waterLevel + o.D.y - (p.camera.y + t * ray.y));
}
float3 vgNormal(OceanPoint o)
{
    // Tangents (1 + dDx/dx, dh/dx, dDz/dx) and (dDx/dz, dh/dz, 1 + dDz/dz).
    return normalize(cross(float3(o.dDdz.x, o.dDdz.y, 1 + o.dDdz.z), float3(1 + o.dDdx.x, o.dDdx.y, o.dDdx.z)));
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
    // Far field: id = quad x 2 + triangle; near field: 0x80000000 | level << 28 | (quad x 2 + triangle).
    const bool isNear = (key.x & 0x80000000u) != 0;
    const uint level = (key.x >> 28) & 7u, local = isNear ? (key.x & 0x0FFFFFFFu) : key.x;
    const uint quad = local >> 1, across = isNear ? viewGridNearLevel(p, level).points - 1 : p.columns;
    const int2 q = int2(quad % across, quad / across);
    const int2 corner[3] = { q, (local & 1) ? q + int2(1, 1) : q + int2(1, 0), (local & 1) ? q + int2(0, 1) : q + int2(1, 1) };
    float2 xy[3], x0v[3];
    float iz[3];
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        int2 fixedXy;
        uint flag;
        if (isNear) viewGridNearVertex(p, level, corner[k], P[0].w, P[1].x, fixedXy, iz[k], flag, x0v[k]);
        else viewGridVertex(p, corner[k], P[0].w, P[1].x, fixedXy, iz[k], flag, x0v[k]);
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
    const float sx = c.x / float(p.width) * 2 - 1 + p.offset.x, sy = 1 - c.y / float(p.height) * 2 + p.offset.y;
    const float3 ray = normalize(p.forward + sx * p.tanX * p.right + sy * p.tanY * p.up);
    const float along = dot(ray, p.forward);
    float t = meshDepth / along;
    float2 x0 = meshX0;
    const float r = distance(meshX0, p.camera.xz);
    // The cell (across, along) and the vertices' band limit: far rows, or the near level's lattice spacing.
    const float nearSpacing = isNear ? viewGridNearLevel(p, level).spacing : 0;
    const float2 footprint = isNear ? float2(nearSpacing, nearSpacing) : float2(r * p.theta, viewGridRow(p, uint(q.y)).y);
    const float filter = max(footprint.x, footprint.y);
    const float pixelFootprint = max(t, 1e-3) * p.theta;
    float meshResidual = 0;
    [loop] for (uint n = 0; n < VG_POLISH; ++n)
    {
        const OceanPoint o = oceanSample(P[0].w, P[1].x, p.lengths, x0, filter);
        const float3 f = vgResidual(p, o, x0, t, ray);
        const bool converged = length(f) <= 0.05 * pixelFootprint;
        if (n == 0)
        {
            meshResidual = abs(dot(float3(f.x, f.z, f.y), vgNormal(o)));  // along the surface normal at x0
        }
        if (converged) break;
        // J = [[1 + dDx/dx, dDx/dz, -v.x], [dDz/dx, 1 + dDz/dz, -v.z], [dh/dx, dh/dz, -v.y]]; solve J delta = -f (Cramer).
        const float3 c0 = float3(1 + o.dDdx.x, o.dDdx.z, o.dDdx.y), c1 = float3(o.dDdz.x, 1 + o.dDdz.z, o.dDdz.y), c2 = -float3(ray.x, ray.z, ray.y);
        const float det = dot(c0, cross(c1, c2));
        if (abs(det) < 1e-8) break;
        const float3 delta = float3(dot(-f, cross(c1, c2)), dot(c0, cross(-f, c2)), dot(c0, cross(c1, -f))) / det;
        x0 += delta.xy;
        t += delta.z;
    }
    const OceanPoint final = oceanSample(P[0].w, P[1].x, p.lengths, x0, filter);
    const float3 residual = vgResidual(p, final, x0, t, ray);
    const bool polished = length(residual) <= 0.05 * pixelFootprint && distance(x0, meshX0) <= 2 * length(footprint) && t > 0;
    const float depth = polished ? t * along : meshDepth;
    surface[pixel] = polished ? float4(x0, depth, 1) : float4(meshX0, depth, 2);
    depthOut[pixel] = depth;
    if (P[1].w)
    {
        RWTexture2D<float> error = ResourceDescriptorHeap[P[1].w];
        error[pixel] = (polished ? abs(dot(float3(residual.x, residual.z, residual.y), vgNormal(final))) : meshResidual) / pixelFootprint;
    }
}
