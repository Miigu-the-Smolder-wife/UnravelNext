// unx-kernel: ps_6_6 main
// r.card.vcapture (surface_cache.mesh_cards_capture_clusters; SurfaceCacheCards.cpp): the card capture drawn through
// V's raster service (FrameServices::rasterizeDepth, Passes/Visibility/DepthRaster.hlsli) - a card page is an
// orthographic view of its instance, drawn from the cluster hierarchy's cut at the page's texel size, as Unreal's Lumen
// captures its cards through Nanite: a dense mesh costs by the page's texels, not by its source triangles
// (CardCapture.ms.hlsl draws those). Each page is a view of the request, its 8-texel tiles sent to the page's place in
// the capture atlas (the service's tile atlas); SV_Position is the capture atlas pixel, z the view's depth: 1 at the
// card's front, 0 at its back (the service's reversed depth), and RasterView::userData = the capture's index in the
// round's list | 0x80000000 where the page's image mirrors the side the card is seen from.
// One run of the service: the request has the capture's depth as its depth target and the three material images as its
// render targets (CardCapture.ps.hlsl states them), so the depth test keeps the surface nearest the card's front and
// its material with it - what the source-triangle capture's pipeline does. A fragment of a cut-out texel, of a class
// the cache does not hold (water, glass, hair) or of the back of a one-sided surface is discarded.
// The surface frame is the service's (DepthRasterRequest::pixelNormals): the vertices' interpolated world normal and
// tangent, brought to the mesh's axes by the transpose of the instance's linear part (the normal exactly; the tangent
// under the uniform scale the cards assume) and from there onto the card's axes, as CardCapture.ms.hlsl lays the
// source vertices' - a smooth-shaded low-polygon mesh has a smooth normal in its cards.
// P[4] = { the round's captures SRV (CardCaptureList.hlsli; DepthRasterRequest::pixelViews), M's texture table SRV
//          (UNX_NONE: none), 0, 0 }
#define DEPTH_RASTER_NORMALS 1
#include "Passes/Visibility/DepthRaster.hlsli"
#include "Passes/SurfaceCache/CardCaptureMaterial.hlsli"
#include "Passes/SurfaceCache/CardCaptureList.hlsli"

struct CardPixel
{
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    float4 emissive : SV_Target2;
};

// A world direction on the mesh's axes: the transpose of the instance's linear part applied to it.
float3 ccToMesh(float4 rows[3], float3 v) { return rows[0].xyz * v.x + rows[1].xyz * v.y + rows[2].xyz * v.z; }

CardPixel main(DepthRasterPixel p, bool front : SV_IsFrontFace)
{
    // (the steps before any flow that depends on the pixel)
    const float2 uvDx = ddx(p.uv), uvDy = ddy(p.uv);
    const float zDx = ddx(p.position.z), zDy = ddy(p.position.z);
    if (!depthRasterCovered(p)) discard;
    const GpuMaterial m = loadMaterial(p.material);
    const uint cls = materialClass(m);
    if (cls == MATERIAL_WATER || cls == MATERIAL_GLASS || cls == MATERIAL_HAIR) discard;
    const bool twoSided = (m.classFlags & MATERIAL_TWO_SIDED) != 0;
    // the side the capture sees: a front face appears counter-clockwise in the page's image, or clockwise where the
    // image mirrors the card's side
    const bool mirrored = (p.userData >> 31) != 0;
    if (!twoSided && front == mirrored) discard;
    const CcCapture cap = ccLoadCapture(P[4].x, p.userData & 0xFFFFu);
    // the fragment in card space: its place on the page, its depth; the steps of a capture pixel (the triplanar
    // materials' footprint)
    const float2 inPage = (p.position.xy - float2(cap.captureOrigin)) / float2(cap.captureSize);
    const float2 uvSize = cap.cardUvRect.zw - cap.cardUvRect.xy;
    const float3 extent = max(cap.cardExtent, 1e-6);
    const float3 card = float3(((cap.cardUvRect.xy + inPage * uvSize) * 2 - 1) * extent.xy, (p.position.z * 2 - 1) * extent.z);
    const float2 texel = 2 * extent.xy * uvSize / float2(cap.captureSize);  // metres a capture pixel along the card's x, y
    const float3 dPdx = float3(texel.x, 0, 2 * extent.z * zDx), dPdy = float3(0, texel.y, 2 * extent.z * zDy);
    float3 ax, ay, az;
    ccAxes(cap.direction, ax, ay, az);
    // the interpolated frame on the card's axes; the bitangent's sign follows the card basis' handedness
    const GpuInstance inst = loadInstance(p.instance);
    const float3 normalMesh = ccToMesh(inst.objectToWorld, p.normal), tangentMesh = ccToMesh(inst.objectToWorld, p.tangent.xyz);
    const float handedness = dot(cross(ax, ay), az);
    CcSurface s;
    s.material = p.material;
    s.tableSrv = P[4].y;
    s.uv = p.uv;
    s.uvDx = uvDx;
    s.uvDy = uvDy;
    s.normal = dot(normalMesh, normalMesh) > 1e-20 ? float3(dot(normalMesh, ax), dot(normalMesh, ay), dot(normalMesh, az)) : float3(0, 0, 1);
    s.tangent = float4(dot(tangentMesh, ax), dot(tangentMesh, ay), dot(tangentMesh, az), p.tangent.w * handedness);
    s.scaled = cap.cardOrigin + ax * card.x + ay * card.y + az * card.z;
    s.scaledDx = ax * dPdx.x + az * dPdx.z;
    s.scaledDy = ay * dPdy.y + az * dPdy.z;
    s.direction = cap.direction;
    s.twoSided = twoSided;
    // (depthRasterCovered settled the cut-out with V's alpha test, at this footprint's steps or the main view's)
    const CcMaterial material = ccMaterial(s);
    CardPixel o;
    o.albedo = material.albedo;
    o.normal = material.normal;
    o.emissive = material.emissive;
    return o;
}
