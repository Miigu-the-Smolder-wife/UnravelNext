// unx-kernel: ps_6_6 main
// unx-variants: PASS=0,1
// r.card.vdepth / r.card.vmaterial (surface_cache.mesh_cards_capture_clusters; SurfaceCacheCards.cpp): the card capture
// drawn through V's raster service (FrameServices::rasterizeDepth, Passes/Visibility/DepthRaster.hlsli) - a card page
// is an orthographic view of its instance, drawn from the cluster hierarchy's cut at the page's texel size, as
// Unreal's Lumen captures its cards through Nanite: a dense mesh costs by the page's texels, not by its source
// triangles (CardCapture.ms.hlsl draws those). Each page is a view of the request, its 8-texel tiles sent to the page's
// place in the capture atlas (the service's tile atlas); SV_Position is the capture atlas pixel, z the view's depth:
// 1 at the card's front, 0 at its back (the service's reversed depth), and RasterView::userData = the capture's index
// in the round's list | 0x80000000 where the page's image mirrors the side the card is seen from.
//   PASS 0  depth only: a fragment of a cut-out texel, of a class the cache does not hold (water, glass, hair) or of
//           the back of a one-sided surface is discarded; the hardware depth keeps the surface nearest the card's front.
//   PASS 1  the material: the same discards, then only the fragment whose depth is the one PASS 0 kept writes the
//           texel's albedo, normal and emission (CardCaptureMaterial.hlsli) - the service's pixel kernels write through
//           UAVs, whose order over a pixel's fragments is not the depth test's, so the nearest surface is settled
//           first and written once.
// The service gives a pixel its uv, material and instance - no normal: the texel's normal is the drawn triangle's, from
// the depth's steps over the page (dz/dx, dz/dy in card space), and the normal map's slope is laid on the frame of the
// uv's steps. Against the source-triangle capture a smooth-shaded low-polygon mesh shows its facets in the card's normal
// (the interface this path asks of V: the interpolated vertex normal and tangent in DepthRasterPixel).
// P[4] = { capture context SRV (raw, persistent), the round's record offset in it (bytes), 0, 0 }; the record (32 B):
//        captures SRV (CardCaptureList.hlsli), PASS 0's depth SRV, albedo UAV, normal UAV, emissive UAV, M's texture
//        table SRV (UNX_NONE: none), 0, 0 - written when the round's passes execute (the graph's views exist then).
#include "Passes/Visibility/DepthRaster.hlsli"
#include "Passes/SurfaceCache/CardCaptureMaterial.hlsli"
#include "Passes/SurfaceCache/CardCaptureList.hlsli"

void main(DepthRasterPixel p, bool front : SV_IsFrontFace)
{
#if PASS == 1
    // (the steps before any flow that depends on the pixel)
    const float2 uvDx = ddx(p.uv), uvDy = ddy(p.uv);
    const float zDx = ddx(p.position.z), zDy = ddy(p.position.z);
#endif
    if (!depthRasterCovered(p)) discard;
    const GpuMaterial m = loadMaterial(p.material);
    const uint cls = materialClass(m);
    if (cls == MATERIAL_WATER || cls == MATERIAL_GLASS || cls == MATERIAL_HAIR) discard;
    const bool twoSided = (m.classFlags & MATERIAL_TWO_SIDED) != 0;
    // the side the capture sees: a front face appears counter-clockwise in the page's image, or clockwise where the
    // image mirrors the card's side
    const bool mirrored = (p.userData >> 31) != 0;
    if (!twoSided && front == mirrored) discard;
#if PASS == 1
    ByteAddressBuffer context = ResourceDescriptorHeap[P[4].x];
    const uint4 c0 = context.Load4(P[4].y);
    const uint2 c1 = context.Load2(P[4].y + 16);
    const uint2 pixel = uint2(p.position.xy);
    Texture2D<float> nearest = ResourceDescriptorHeap[c0.y];
    if (abs(p.position.z - nearest.Load(int3(pixel, 0))) > 2e-6) discard;
    const CcCapture cap = ccLoadCapture(c0.x, p.userData & 0xFFFFu);
    // the fragment in card space: its place on the page, its depth; the steps of a capture pixel
    const float2 inPage = (p.position.xy - float2(cap.captureOrigin)) / float2(cap.captureSize);
    const float2 uvSize = cap.cardUvRect.zw - cap.cardUvRect.xy;
    const float3 extent = max(cap.cardExtent, 1e-6);
    const float3 card = float3(((cap.cardUvRect.xy + inPage * uvSize) * 2 - 1) * extent.xy, (p.position.z * 2 - 1) * extent.z);
    const float2 texel = 2 * extent.xy * uvSize / float2(cap.captureSize);  // metres a capture pixel along the card's x, y
    const float3 dPdx = float3(texel.x, 0, 2 * extent.z * zDx), dPdy = float3(0, texel.y, 2 * extent.z * zDy);
    // the triangle's normal, toward the capture side (z > 0)
    const float3 n = normalize(float3(-dPdx.z * texel.y, -dPdy.z * texel.x, texel.x * texel.y));
    // the tangent frame of the uv's steps (dP/du, dP/dv)
    const float det = uvDx.x * uvDy.y - uvDy.x * uvDx.y;
    float4 tangent = 0;
    if (abs(det) > 1e-20)
    {
        const float3 T = (dPdx * uvDy.y - dPdy * uvDx.y) / det, B = (dPdy * uvDx.x - dPdx * uvDy.x) / det;
        tangent = float4(T, dot(cross(n, T), B) < 0 ? -1.0 : 1.0);
    }
    float3 ax, ay, az;
    ccAxes(cap.direction, ax, ay, az);
    CcSurface s;
    s.material = p.material;
    s.tableSrv = c1.y;
    s.uv = p.uv;
    s.uvDx = uvDx;
    s.uvDy = uvDy;
    s.uv1 = p.uv;  // (the service gives a pixel one uv set and no vertex colour: the second set is the first, the colour white)
    s.uv1Dx = uvDx;
    s.uv1Dy = uvDy;
    s.color = 1;
    s.normal = n;
    s.tangent = tangent;
    s.scaled = cap.cardOrigin + ax * card.x + ay * card.y + az * card.z;
    s.scaledDx = ax * dPdx.x + az * dPdx.z;
    s.scaledDy = ay * dPdy.y + az * dPdy.z;
    s.direction = cap.direction;
    s.twoSided = twoSided;
    // (PASS 0 settled the texel's surface with V's alpha test: its material is written whatever this footprint's test says)
    const CcMaterial material = ccMaterial(s);
    RWTexture2D<float4> albedo = ResourceDescriptorHeap[c0.z];
    RWTexture2D<float2> normal = ResourceDescriptorHeap[c0.w];
    RWTexture2D<float3> emissive = ResourceDescriptorHeap[c1.x];
    albedo[pixel] = material.albedo;
    normal[pixel] = material.normal;
    emissive[pixel] = material.emissive.rgb;
#endif
}
