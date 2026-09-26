#pragma once
// Per-view products (ViewResources) and view-independent products (FrameResources) of a frame: the tracks' shared
// board (INTERFACES_KO.md 5, 7). Split from Frame.h (v1.42); Frame.h still includes it.
#include "unx/render/GraphTypes.h"
#include "unx/render/ViewDesc.h"

#include <vector>

namespace unx::render
{
// Per-view products (graph resources of the current frame). Producer in brackets.
struct ViewResources
{
    ViewDesc view;
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;  // root CBV b1 for passes of this view [core]
    TextureRef depth;              // D32_FLOAT reversed Z                                   [V]
    TextureRef visId;              // R32_UINT (VisBuffer.hlsli)                             [V]
    BufferRef visibleClusters;     // gpu::VisibleCluster list indexed by the vis id         [V]
    TextureRef hiz;                // R32_FLOAT, full mip chain: texel (i, j) of mip m = farthest   [V]
                                   // depth (minimum reversed-Z value) of the pixels [i 2^(m+1), ...);
                                   // valid mip size ceil(W / 2^(m+1)) x ceil(H / 2^(m+1)) inside a
                                   // power-of-two allocation (texels beyond it are undefined)
    // Coverage layer (7.1 v1.41, CoverageTiles.hlsli; invalid = no layer), per 8 x 8 tile:
    BufferRef coverageTiles;       // raw: 8-word tile headers (records, record base, listed index + 1, [V]
                                   // opaqueCovered 64 bit, V-internal words)
    BufferRef coverageRecords;     // StructuredBuffer<uint4>: each listed tile's records contiguous,  [V]
                                   // pixel-major inside the tile's range
    BufferRef coverageTileList;    // raw: header (args over the listed tiles and over the blocks,     [V]
                                   // counts, capacity, tiles per row), 4 words per listed tile
                                   // { tile, records, record base, block base }
    BufferRef coverageTilePixels;  // raw: 64 words per listed tile, pixel p's first record in the  [V]
                                   // tile's range
    TextureRef coverageDepthRange; // R32G32_UINT per pixel: its records' nearest (max) and farthest  [V]
                                   // (min) depth bits, see-through included; (0, 0xFFFFFFFF) = none
    BufferRef coverageChunkTable;  // v1.40 names until M's composite reads the ranges: the table is   [V]
    BufferRef coverageChunks;      // always invalid (turns the v1.40 readers off), chunks = records
    TextureRef gbuffer;            // RG32_UINT (GBuffer.hlsli)                              [M]
    TextureRef shadowVisibility;   // R32_UINT, 4 light slots x 8 bit (7.3)                 [S]
    TextureRef shadowOverflowTiles;  // R32_UINT ceil(W/8) x ceil(H/8) (main view, 7.3, v1.20): [S]
                                     // 0 = no shadow-casting light past the third in the tile,
                                     // 0xFFFFFFFF = over capacity (fallback list), else 1 + the
                                     // tile's block start word in shadowOverflow
    BufferRef shadowOverflow;      // raw: per overflow tile 64 pixel words (count << 24 | run  [S]
                                   // start) + runs of 8-bit visibilities, list order (7.3)
    BufferRef shadowOverflowFallbackTiles;  // raw: word 0 count, words 1..3 DispatchIndirect  [S]
                                            // args (count, 1, 1), words 4.. tiles (y << 16 | x)
    BufferRef shadowFragmentVisibility;  // StructuredBuffer<uint3> per pixel (y x width + x), valid  [S]
                                         // where coverageDepthRange has records: x = sun visibility
                                         // at 4 points of [nearest, farthest] (unorm8 each), y = local
                                         // slots 1..3 at the nearest (byte 0 bit 0: pair flag), z = the
                                         // same at the farthest (7.3, v1.41)
    BufferRef shadowFragmentSun;   // raw, 1 B per coverageRecords element: that record's sun  [S]
                                   // visibility (unorm8); written only for pair-flag pixels (7.3)
    BufferRef froxelLights;        // this view's froxel light lists (7.4; v1.22): main view =      [S]
                                   // FrameResources::froxelLights, planar views: S shadowVisibility
    TextureRef airVolume;          // this view's air volume (v1.15 layout; v1.22): main view =     [S]
                                   // FrameResources::aerialPerspective; planar views integrate from
                                   // the mirror plane on (the main view's mirror pixel has the rest)
    TextureRef screenProbes;       // GI screen probes (main view only)                     [R]
    TextureRef screenProbeMaps;    // atlas of the K-path radiance maps of the cache entries  [R]
                                   // the screen probes use, hardware-filterable (M: SrvCompute; R's
                                   // ScreenProbes.hlsli defines the layout; v1.13)
    BufferRef screenProbeBlocks;   // GI screen probe blocks, 640 B per corner probe (main view   [R]
                                   // only; v1.36): (ceil(W/8) + 1) x (ceil(H/8) + 1) probes, each
                                   // contiguous: records 0..79, mips 0/1/2 fp16 RGB 80..583, spare
                                   // (the table R and M agreed); M reads SrvCompute (ProbeSrvs)
    TextureRef reflection;         // RGBA16F reflection radiance + weight (main view only) [R]
    TextureRef reflectionLobeTiles;  // R8_UNORM ceil(W/8) x ceil(H/8): min over the tile's      [M]
                                     // surface pixels of reflectionLobeHalfAngle(r, NoV) / pi
                                     // (Reflection.hlsli; sky-only tile = 1); R skips ray
                                     // classification in tiles whose minimum is K-path wide
    // Particle layer (FX request 20260926_FX_particle_render_pass 8a; Passes/FX/ParticleLayer.hlsli; invalid = none):
    TextureRef particleLayer;      // RGBA16F, 1/4 resolution: premultiplied radiance + transmittance   [FX]
    TextureRef particleDepthRange; // RG16F, 1/4 resolution: the layer's depth range per texel          [FX]
    BufferRef particleEdges;       // raw: full-resolution edge pixels of the layer + count              [FX]
    // Heat haze (FEATURES_GAME 0.A-8; E's Passes/Volume; invalid = none): M re-reads the HDR target at p + D x (1 - z_p / z_b)
    // for pixels behind the haze (z_b: the pixel's view depth, z_p: distortionDepth's view depth).
    TextureRef distortionOffset;   // RG16F, ceil(W/4) x ceil(H/4): deflection D in full-resolution pixels (far-field    [E]
                                   // limit of the bent view ray; linear sum of the particles' index gradients, exact
                                   // while |D| < 8 px)
    TextureRef distortionDepth;    // R16F, same size: device depth (reversed Z) of the nearest haze particle's front;  [E]
                                   // 0 = no haze; a pixel is displaced when its opaque depth is below it (behind)
    TextureRef volumeSlices;       // RGBA16F gridX x gridY x 2S on the froxel grid (main view): part 0 the particle      [E]
                                   // media's optical depth of each slice (rgb), part 1 its source (nit, before exposure,
                                   // at the slice entry, self-attenuated); S adds them in the froxel integration
    // Projected decals (FEATURES_GAME 5, E's Passes/Decal; invalid = none this frame): M's resolve (and R's hit shading)
    // pass both to decalApply (Decal.hlsli).
    BufferRef decalFrames;         // StructuredBuffer<DecalFrame>: the frame's decals, camera-relative               [E]
    BufferRef decalTiles;          // raw: 16 x 16 px tile lists (<= 16 decals per tile, header + status)             [E]
    TextureRef color;              // final colour target of this view                      [M]
};

// A triangle stream the GPU makes in the frame (INTERFACES v1.60; W's water surface and fluid surface, request
// 20260926_W_gpu_triangle_stream): V draws it into the main view's coverage layer as see-through records (band A keeps
// what lies beneath). Non-indexed triangles, 32 B per vertex: (world position, 1), (normal, 0), counter-clockwise seen
// from outside; the triangle count is 1/3 of drawArgs' vertex count (D3D12_DRAW_ARGUMENTS, written on the GPU), at most
// maxTriangles (the vertex buffer's capacity: V's dispatch covers it). velocities (optional, float4 per vertex, world m/s):
// M's motion (previous position = position - velocity x delta time).
struct TriangleStream
{
    BufferRef vertices, drawArgs, velocities;
    uint32_t material = 0;              // scene material (M's Water class)
    uint32_t instance = 0xFFFFFFFFu;    // scene instance it belongs to, or none
    uint32_t maxTriangles = 0;
    float3 boundsMin{}, boundsMax{};    // world AABB (culling)
};
constexpr uint32_t kMaxTriangleStreams = 64;  // vis id slot bits (CoverageTiles.hlsli COV_STREAM_ID)

// View-independent products of the current frame. Persistent state (VSM pool, GI cache, TLAS) is imported into the
// graph each frame by its owner.
struct FrameResources
{
    TextureRef transmittanceLut, multiScatterLut, skyViewLut;  // [S]
    TextureRef aerialPerspective;  // the air volume (froxels(), v1.15): Texture3D RGBA16F      [S]
                                   // gridX x gridY x 3(S+1) + 2 on the froxel grid, part 0 in-scattering
                                   // camera -> node (x exposure; atmosphere, caster-shadowed air, local
                                   // lights, E's particle media), part 1 optical depth, part 2 sun
                                   // transmittance at the node, then the sky correction and the media's
                                   // optical depth to far_m (sky pixels); read with atmosphereAerial /
                                   // atmosphereAirView / atmosphereSkyRadianceView (Atmosphere.hlsli)
    BufferRef vsmPool;             // physical page pool (raw buffer; v1.18); replaced by vsmAtlas [S]
                                   // (v1.43): invalid once S publishes the atlas
    TextureRef vsmAtlas;           // v1.43 (S request 20260926_S_vsm_one_path): the page atlas,  [S]
                                   // D32_FLOAT, SRV R32_FLOAT, page p at ((p % 128), (p / 128)) x
                                   // 128 px, 0 = no caster; ShadowSrvs.pool carries its SRV. Readers
                                   // move to it while vsmPool is still valid; S switches when M, R
                                   // and FX read it, then vsmPool goes (no window where a buffer
                                   // descriptor is read as a texture)
    BufferRef vsmPageTable;        //                                                       [S]
    BufferRef vsmBlocks;           // per-page block hierarchy (persistent; v1.18)          [S]
    BufferRef vsmSearchBound;      // blocker-search bound grid of this frame (v1.18)       [S]
    BufferRef vsmLayers;           // VSM transmittance layer (raw; v1.26, S request): per physical [S]
                                   // page its layer + 1 (0 = none, T = 1), then the layer pages (4
                                   // knots per texel) and block profiles; ShadowSrvs.layers
    uint32_t vsmConstants = UINT32_MAX;  // CBV descriptor of this frame's VSM constants    [S]
    // v1.45 (B2, COVERAGE 12.4 structure 2): raw SRV of 1 bit per scene light, set when the light's revision has held for  [M]
    // >= 8 frames and raytracing.emitters is on. M leaves those area lights' LTC specular to R's reflection paths (K/G/M
    // see the emitters); R excludes the unset ones from direct emitter hits, so no light is counted twice or missed.
    // UINT32_MAX when raytracing.emitters is off (M evaluates every area light's specular).
    uint32_t areaLightStable = UINT32_MAX;
    // v1.49 (B4): raw SRV of this frame's celestial record (Celestial.hlsli atmosphereCelestial; upload ring, not a graph  [S]
    // resource); UINT32_MAX when FrameContext::celestial draws nothing.
    uint32_t celestial = UINT32_MAX;
    // v1.50 (B6): raw SRV of this frame's wind header (WindCache.hlsli: the cache grid's origin and spacing, the cache      [S]
    // texture's SRV, the records' SRV and count, the tick time) for windSample / windExact; UINT32_MAX without records.
    uint32_t wind = UINT32_MAX;
    TextureRef windCache;          // v1.50 (B6): the wind cache the header's texture SRV names (declare it to sample)  [S]
    // v1.50 (B6): raw SRV of this frame's weather record (WeatherField.hlsli: the World's weather row, rainExposure's map    [S]
    // parameters) and the rain shadow map it names (declare rainShadow to read rainExposure); UINT32_MAX / invalid without weather.
    uint32_t weather = UINT32_MAX;
    TextureRef rainShadow;
                                         // (upload ring, not a graph resource; v1.18). With
                                         // the four buffers: ShadowSrvs (ShadowVisibility.hlsli),
                                         // filled by shadowPages for shadowSunVisibilityAt (R)
    // Surface state field (E's Passes/Decal SurfaceState.hlsli; invalid = none): surfaceStateAt reads the three (raw).  [E]
    BufferRef surfaceConstants, surfaceTable, surfacePool;
    // Strand hair (E's Passes/Hair, unx/hair/Hair.h; invalid = no hair): the frame's follow-strand segments (2 float4 each:  [E]
    // camera-relative p0, r0; p1, r1; r = 0 left out by LOD) and the bodies' header (raw) for V's coverage layer and M.
    BufferRef hairSegments, hairBodies;
    // GPU triangle streams of this frame (W, before V: tracks::waterGeometry; V's coverage layer, v1.60).  [W]
    std::vector<TriangleStream> triangleStreams;
    // Light functions (E's Passes/Lights LightFunction.hlsli, A8; invalid = no light has one): cookies, IES, gobos,  [E]
    // flicker and animation per light index. Every reader of a light's emission (M shading, S froxel in-scattering, R
    // hit shading and GI) multiplies it by lightFunction(srv, light, forward, right, dir, footprint, g_time) (raw).
    BufferRef lightFunctions;
    uint32_t vsmLocalLights = UINT32_MAX;  // SRV descriptors of this frame's local-light shadow [S]
    uint32_t vsmSlotOfLight = UINT32_MAX;  // records (VsmLocalLight, 48 B x shadow slots) and the
                                           // scene light -> shadow slot table (uint, 0xFFFF = none)
                                           // (upload ring; v1.19): ShadowSrvs.lights / .pad0 for
                                           // shadowVisibilityDirect
    TextureRef froxels;            // the same air volume as aerialPerspective (v1.15)      [S]
    BufferRef froxelLights;        // per-froxel light lists (7.4)                          [S]
    BufferRef tlasStatic, tlasDynamic;  // acceleration structures                          [R]
    BufferRef giCache;             // world radiance cache                                  [R]
};
} // namespace unx::render
