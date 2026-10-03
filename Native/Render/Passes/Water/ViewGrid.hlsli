// Water view grid (FEATURES_GAME 1.8 B): the camera's water surface at the density its visible detail needs. Far field:
// a grid of the camera's world azimuth phi (spacing theta: one pixel across) x rest distance r_j from the camera (a row
// table: spacing min(one pixel row on the still plane, 2.5 pixel footprints), because a crest's height projects to
// a / f pixels while its length along the view is foreshortened: a crest 0.5 px tall (wavelength >= 10 f in the spectrum
// tail) needs samples 2.5 f apart along the view). The displaced grid is scattered into the pixels with a 64-bit atomic
// min of (view depth | triangle id): each pixel's first intersection with the displaced mesh.
// Parameter buffer (raw):
//   float4 rows 0 camera position (x, y, z), water level        1 right (x, y, z), tan(fov x / 2)
//               2 up (x, y, z), tan(fov y / 2)                  3 forward (x, y, z), far ring (m: the water body's extent)
//               4 phi_0 (rad: the first column), projection offset x (NDC), theta (rad), near-field radius r_n (m, horizontal)
//               5 columns, rows, width, height (uint)
//               6 cascade lengths (m) 0..2, near plane (m: view depth; points nearer are invalid)
//               7 mask: lake centre x, z, radius (m), enabled (uint; 0 = open sea)
//               8 near-field levels K (uint), spacing coefficient (s(t) = coefficient sqrt(t)), distance floor t_floor (m),
//                 projection offset y (NDC; the offset: ViewGrid.h ViewGridCamera::offset - the frame's sub-pixel jitter)
//   from byte 144, per near level i (32 B): (inner radius, outer radius (m, horizontal), spacing s_i (m), points per side
//               n_i (uint)), (lattice origin x, z (int: point (a, b) is at (origin + (a, b)) s_i), 0, 0)
//   byte 400: the screen's angular window (azimuth min, max, elevation min, max; rad)
//   byte 416: diagnostics' drawn-block list UAV (raw: count, then (level, block x, z) from byte 16; 0 = none)
//   from byte 512: per far row j, (r_j, along spacing at r_j) float2
#ifndef UNX_WATER_VIEW_GRID_HLSLI
#define UNX_WATER_VIEW_GRID_HLSLI
#include "Bindless.hlsli"
#include "OceanSample.hlsli"

#define VG_TRIANGLE_PIXELS 8  // pixels per triangle and axis tested in the scatter thread (structural; excess counted)
#define VG_ROW_TABLE 512u     // byte offset of the row table in the parameter buffer
#define VG_NEAR_LEVELS 8u     // at most (ids: level in bits 28..30 of a near-field triangle id)

struct ViewGridParams
{
    float3 camera; float waterLevel;
    float3 right; float tanX;
    float3 up; float tanY;
    float3 forward; float far;
    float phi0, theta, nearRadius;
    uint columns, rows, width, height;
    float3 lengths;
    float nearPlane;
    float2 lakeCentre; float lakeRadius; uint lake;
    float2 offset;
    uint srv;
};
ViewGridParams viewGridParams(uint srv)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    ViewGridParams p;
    float4 r;
    r = asfloat(b.Load4(0));   p.camera = r.xyz; p.waterLevel = r.w;
    r = asfloat(b.Load4(16));  p.right = r.xyz; p.tanX = r.w;
    r = asfloat(b.Load4(32));  p.up = r.xyz; p.tanY = r.w;
    r = asfloat(b.Load4(48));  p.forward = r.xyz; p.far = r.w;
    r = asfloat(b.Load4(64));  p.phi0 = r.x; p.theta = r.z; p.nearRadius = r.w;
    p.offset = float2(r.y, asfloat(b.Load(140)));
    const uint4 u = b.Load4(80); p.columns = u.x; p.rows = u.y; p.width = u.z; p.height = u.w;
    r = asfloat(b.Load4(96));  p.lengths = r.xyz; p.nearPlane = r.w;
    const uint4 m = b.Load4(112); p.lakeCentre = asfloat(m.xy); p.lakeRadius = asfloat(m.z); p.lake = m.w;
    p.srv = srv;
    return p;
}
struct ViewGridNearLevel
{
    float inner, outer, spacing;
    uint points;
    int2 origin;
};
uint viewGridNearLevels(ViewGridParams p)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[p.srv];
    return b.Load(128);
}
ViewGridNearLevel viewGridNearLevel(ViewGridParams p, uint level)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[p.srv];
    const uint4 r0 = b.Load4(144 + 32 * level), r1 = b.Load4(160 + 32 * level);
    ViewGridNearLevel l;
    l.inner = asfloat(r0.x); l.outer = asfloat(r0.y); l.spacing = asfloat(r0.z); l.points = r0.w;
    l.origin = asint(r1.xy);
    return l;
}
float viewGridNearCoefficient(ViewGridParams p)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[p.srv];
    return asfloat(b.Load(132));
}
float viewGridNearFloor(ViewGridParams p)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[p.srv];
    return asfloat(b.Load(136));
}
uint viewGridDrawnList(ViewGridParams p)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[p.srv];
    return b.Load(416);
}
float4 viewGridWindow(ViewGridParams p)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[p.srv];
    return asfloat(b.Load4(400));
}
// Row j: (rest distance r_j, spacing along the view there).
float2 viewGridRow(ViewGridParams p, uint row)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[p.srv];
    return asfloat(b.Load2(VG_ROW_TABLE + 8 * min(row, p.rows - 1)));
}

// Rest position (x, z) of grid point (column i, row j) and its horizontal distance; false with the camera under water.
// The row table ends at the far ring (the water body's extent; open sea: the horizon), so the mesh reaches it.
bool viewGridRest(ViewGridParams p, int2 ij, out float2 x0, out float distance)
{
    const float phi = p.phi0 + float(ij.x) * p.theta;
    distance = viewGridRow(p, uint(ij.y)).x;
    x0 = p.camera.xz + distance * float2(cos(phi), sin(phi));
    return p.camera.y > p.waterLevel;
}
// Rest-plane footprint of a grid cell (m): across (azimuth) d theta, along (the view direction) the row spacing.
float2 viewGridFootprint(ViewGridParams p, float distance, int row)
{
    return float2(distance * p.theta, viewGridRow(p, uint(row)).y);
}
// Displacement (Dx, h, Dz) at rest position x0 for the isotropic footprint s: OceanSample.hlsli (C1 below a texel).
float3 viewGridDisplacement(uint field, uint slopes, float3 lengths, float2 x0, float s)
{
    return oceanSample(field, slopes, lengths, x0, s).D;
}
// The trilinear filter alone (the microbench's sampling floor).
float3 viewGridDisplacementTrilinear(uint field, float3 lengths, float2 x0, float s)
{
    Texture2DArray<float4> f = ResourceDescriptorHeap[field];
    float3 d = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float L = lengths[c];
        const float mip = clamp(log2(s * 512.0 / L), 0.0, 9.0);
        d += f.SampleLevel(g_linearWrap, float3(x0 / L + 0.5 / 512.0, c), mip).xyz;
    }
    return d;
}
// The same with the anisotropic footprint (across, along in rest-plane metres, along the direction `along`): SampleGrad
// on the anisotropic-16 sampler.
float3 viewGridDisplacementAniso(uint field, float3 lengths, float2 x0, float2 footprint, float2 along)
{
    Texture2DArray<float4> f = ResourceDescriptorHeap[field];
    const float2 across = float2(-along.y, along.x);
    float3 d = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float L = lengths[c];
        d += f.SampleGrad(g_anisoWrap, float3(x0 / L + 0.5 / 512.0, c), across * footprint.x / L, along * footprint.y / L).xyz;
    }
    return d;
}
// Screen position (pixels, x right, y down) and view depth of a world point; depth <= 0 behind the camera.
float3 viewGridProject(ViewGridParams p, float3 world)
{
    const float3 v = world - p.camera;
    const float z = dot(v, p.forward);
    const float x = dot(v, p.right) / (z * p.tanX) - p.offset.x, y = dot(v, p.up) / (z * p.tanY) - p.offset.y;
    return float3((x * 0.5 + 0.5) * float(p.width), (0.5 - y * 0.5) * float(p.height), z);
}
// How far a displacement of at most `bound` moves a rest point's direction at horizontal distance r: in elevation at most
// the sphere's angular radius asin(bound / D), D = |(r, h)| (4 rad: no culling when the camera is inside the sphere); in
// azimuth at most asin(bound / r) about the vertical axis - near the nadir a small horizontal move turns the azimuth far
// (4 rad when bound >= r).
float viewGridWiden(ViewGridParams p, float r, float bound)
{
    const float h = p.camera.y - p.waterLevel, distance = sqrt(r * r + h * h);
    return bound >= distance ? 4.0 : asin(bound / distance);
}
float viewGridWidenAzimuth(float r, float bound)
{
    return bound >= r ? 4.0 : asin(bound / r);
}
// Whether the water body reaches within `reach` of rest position x0 (its signed distance <= reach). With reach = the
// grid cell's diagonal, every triangle that touches the water has a vertex that passes: the surface is kept up to one
// cell past the shore, where the terrain covers it.
bool viewGridWater(ViewGridParams p, float2 x0, float reach)
{
    return p.lake == 0 || distance(x0, p.lakeCentre) - p.lakeRadius <= reach;
}
// Grid point q as a scatter vertex: screen position in 1/256 px, 1 / view depth and the flag (0 invalid, 1 inside the
// water body, 2 outside). The displacement is low-passed isotropically to the cell's longer side (at most 2.5 pixel
// footprints: what that removes across the view is below 0.25 px; OceanSample.hlsli), so every evaluation of the same
// point - the owning group, a neighbouring group's halo, the resolve - runs the same instructions on the same inputs
// (the scatter's watertightness rests on it; the hole gate checks it on every device).
void viewGridVertex(ViewGridParams p, int2 q, uint field, uint slopes, out int2 xy, out float inverseDepth, out uint flag, out float2 x0)
{
    xy = 0;
    inverseDepth = 0;
    flag = 0;
    x0 = 0;
    float d;
    if (any(q >= int2(p.columns, p.rows)) || !viewGridRest(p, q, x0, d)) return;
    const float2 footprint = viewGridFootprint(p, d, q.y);
    const float3 disp = viewGridDisplacement(field, slopes, p.lengths, x0, max(footprint.x, footprint.y));
    const float3 s = viewGridProject(p, float3(x0.x + disp.x, p.waterLevel + disp.y, x0.y + disp.z));
    if (s.z <= p.nearPlane) return;
    xy = int2(round(clamp(s.xy, -4.0e6, 4.0e6) * 256.0));
    inverseDepth = 1.0 / s.z;
    flag = viewGridWater(p, x0, length(footprint)) ? 1 : 2;
}
// Near-field point q of level `level` as a scatter vertex (the same outputs): rest position (origin + q) s_i, the
// displacement low-passed to s_i (the level's own sampling; its spacing keeps the linear interpolation error of the
// finest band within 0.5 px, FEATURES_GAME 1.8 B.2). Flag 1 = inside the level's ring and the water body (a triangle
// with such a vertex is drawn, so neighbouring rings and the far field overlap by a cell).
void viewGridNearVertex(ViewGridParams p, uint level, int2 q, uint field, uint slopes, out int2 xy, out float inverseDepth, out uint flag, out float2 x0)
{
    xy = 0;
    inverseDepth = 0;
    flag = 0;
    const ViewGridNearLevel l = viewGridNearLevel(p, level);
    x0 = float2(l.origin + q) * l.spacing;
    if (any(q >= int(l.points)) || p.camera.y <= p.waterLevel) return;
    const float3 disp = viewGridDisplacement(field, slopes, p.lengths, x0, l.spacing);
    const float3 s = viewGridProject(p, float3(x0.x + disp.x, p.waterLevel + disp.y, x0.y + disp.z));
    if (s.z <= p.nearPlane) return;
    xy = int2(round(clamp(s.xy, -4.0e6, 4.0e6) * 256.0));
    inverseDepth = 1.0 / s.z;
    const float r = distance(x0, p.camera.xz);
    flag = (r >= l.inner && r <= l.outer && viewGridWater(p, x0, l.spacing * 1.4142136)) ? 1 : 2;
}
#endif
