#pragma once
// Emissive surfaces as area lights (RENDERER_REDESIGN_V2 14.1b, L2b; owner A). A light ceiling, a light panel or any
// large emitter is a light: its direct view stays the material's emission (ShadeOpaque), and the light it sends to other
// surfaces goes through the direct-light path from the first frame instead of arriving only through the GI cache's
// emission texels (the cold-start black holes and blotches of judge2). The cache then carries the bounces after the
// first alone (the double-counting rule below).
//
// Cook (cookEmissiveLights, once per scene revision): the emissive triangles of the non-skinned instances whose material
// has a constant emission (an emissive texture keeps the material on the cache path: its average is a later cook item)
// are grouped by plane (normal within 1e-4, offset within planeTolerance; two-sided materials emit on both sides, so
// they give two planes) and each plane gets a quadtree of axis-aligned squares on the plane: node radiance = the
// area-weighted mean emission of the triangles clipped to the node (exact polygon clipping; a node holds its content's
// flux exactly as a uniform emitter of its own area), leaves of at most leafSize, nodes without content pruned. No limit
// on the node count (memory 48 B per node).
// Runtime (EmissiveLights.hlsli): a receiver walks each plane's tree from the root and evaluates a node as one Lambert
// polygon when its diagonal a_n <= 0.8 d (d = distance to the node: the averaging error of a planar Lambert emitter's
// pattern at distance d is <= exp(-2 pi d / a) <= 3.9e-4 < 1e-3, whatever the pattern), else descends; leaves are
// evaluated as they are. Nodes whose peak illuminance L A / d^2 is below the exposure floor (1e-3 x the frame's
// mid-grey illuminance: under half a display code) are skipped (the per-node absolute rule; 14.1b's cumulative
// relative rule follows with the tile term L2).
// Double counting (12.4 structure 2, V2.4 13.3): the converted materials are listed in the image's bitset; the GI
// update rays and the reflection hits are to record emission 0 for them (R / S2 apply it in GI/* and HitShading) and
// the emissive-triangle MIS list leaves them out. Shadows of the node lights: 14.3 (L3); this version lights without
// them, so it is behind shading.emissive_area_lights (default off).
#include "unx/render/Frame.h"
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <vector>

namespace unx::lights
{
struct EmissiveCookConfig
{
    float planeTolerance = 0.01f;  // m: triangles within this of a plane join it
    float leafSize = 0.1f;         // m: the largest leaf square
    uint32_t maxDepth = 14;        // quadtree depth limit (root square / 2^depth >= leaf size otherwise)
};

struct EmissivePlane
{
    float3 origin, right, up, normal;  // orthonormal basis on the plane (origin = the root square's centre)
    uint32_t rootNode = 0xFFFFFFFFu;   // index into EmissiveCook::nodes
    double flux = 0;                   // sum over the plane's triangles of luminance x area (cook check)
};

struct EmissiveNode
{
    float2 centre;        // plane coordinates (right, up) relative to the plane origin
    float half;           // half size of the square (m)
    float3 radiance;      // nits, the node's area-weighted mean emission (0 = pruned, never stored)
    float luminance;      // of radiance (Rec. 709)
    uint32_t plane;
    uint32_t firstChild;  // 4 consecutive children (quadrants -x-y, +x-y, -x+y, +x+y; a missing one = 0xFFFFFFFF), or
                          // 0xFFFFFFFF for a leaf
};

struct EmissiveCook
{
    std::vector<EmissivePlane> planes;
    std::vector<EmissiveNode> nodes;
    std::vector<uint32_t> convertedMaterials;  // bitset over Scene::materials (bit = material converted to node lights)
    uint32_t trianglesConverted = 0, trianglesTextured = 0, trianglesSkinned = 0;
    double fluxIn = 0, fluxOut = 0;  // luminance x area of the converted triangles, and of the leaves (equal: check)
};

EmissiveCook cookEmissiveLights(const scene::Scene& scene, const EmissiveCookConfig& config);

// GPU image of a cook (EmissiveLights.hlsli): raw buffer, header 32 B { planeCount, nodeCount, planesOffset,
// nodesOffset, bitsetOffset, bitsetWords, leafSize bits, 0 }, planes 64 B, nodes 48 B, bitset.
std::vector<uint32_t> emissiveLightsImage(const EmissiveCook& cook);

// Per-frame record (M calls it from the shading record): the cook of the frame's scene revision, uploaded once, as a
// raw SRV buffer; invalid when shading.emissive_area_lights is off or the scene has no convertible emitter.
unx::render::BufferRef emissiveLights(unx::render::FramePassContext& fc);
} // namespace unx::lights
