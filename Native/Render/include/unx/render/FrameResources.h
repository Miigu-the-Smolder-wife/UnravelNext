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
    BufferRef exposureCorrection;  // raw 16 B { float c, metered EV100, metered, 0 }: a snap frame's own metering; the  [M]
                                   // output multiplies the exposed image by c (Exposure.h exposureMeter); invalid otherwise
    uint32_t viewId = 0;           // A14: the view's stable id (0 = main; FrameContext::auxViews ids); tracks key their
                                   // per-view histories and persistent resources by it (Requests/20260926_C_per_view_history)
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
    BufferRef coverageSpecial;     // raw (v1.73): header { count, DispatchIndirect args (64 per group, 1, 1) }, then per  [V]
                                   // special record uint2 { coverageRecords element, kind } (1 hair, 2 triangle stream,
                                   // 5 M pre-shaded cluster class: COV_PRESHADE_ID);
                                   // no defined order; past the capacity: OVERFLOW_COVERAGE_SPECIAL (it grows)
    BufferRef coverageRecordRadiance;  // raw (v1.75), 8 B per coverageRecords element: uint2 { f16 r | f16 g << 16,  [M]
                                   // f16 b } exposed linear radiance with aerial perspective (the composite's unit) of the
                                   // records listed in coverageSpecial, written by their owners before the composite:
                                   // M kind 5 (and 0 for kinds 1, 2 before tracks::water), W kind 2 in tracks::water
    TextureRef bandARadiance;      // RGBA16F (v1.75): M's band A exposed radiance kept for edge and coverage-tile pixels,  [M]
                                   // what the E and coverage composites put behind fragments; W writes its interior water
                                   // pixels here too (tracks::water), so fragments over water composite over the water
    TextureRef coverageDepthRange; // R32G32_UINT per pixel: its records' nearest (max) and farthest  [V]
                                   // (min) depth bits, see-through included; (0, 0xFFFFFFFF) = none
    BufferRef coverageChunkTable;  // v1.40 names until M's composite reads the ranges: the table is   [V]
    BufferRef coverageChunks;      // always invalid (turns the v1.40 readers off), chunks = records
    // A6 translucent layer (v1.67; main view with the coverage layer on and a glass or water material in the scene;
    // invalid = none): band A width clusters of the glass and water material classes are not in band A (it keeps what lies
    // behind them); the nearest of them in front of band A is one sample per pixel, and the pixels where that sample is not
    // the whole story get the translucent surfaces as see-through coverage records instead.
    TextureRef translucentVis;     // R32_UINT vis id (VisBuffer.hlsli, this view's visibleClusters) of the nearest   [V]
                                   // translucent surface in front of band A at the pixel centre; VIS_NONE = none
    TextureRef translucentDepth;   // R32_FLOAT its linear view depth (+inf = none)                                   [V]
    TextureRef translucentClass;   // R8_UINT 0 = none; 1 = the sample is the pixel's only translucent surface and   [V]
                                   // covers the whole pixel; 2 = the pixel's translucent surfaces are coverage records
                                   // (see-through, exact area and mask; edges on both sides of an outline, seams,
                                   // overlaps), the sample is unused (it may be none)
    // v1.73 ocean edges (W's view grid): the water layer's ocean edge pixels for W's subsample pass, raw: header    [V]
    // { count, DispatchIndirect args (64 pixels per group), 1, 1 }, then y << 16 | x per pixel; and what that pass needs to
    // append coverage records (CoverageLayer.hlsli coverageAppend) inside FrameServices::coverageAppend.
    BufferRef oceanEdgePixels;
    BufferRef coverageState, coverageStream, coverageKeys;  // UAV targets of coverageAppend (valid inside the hook)   [V]
    uint32_t coverageCapacity = 0, coverageTilesX = 0;
    TextureRef waterVis, waterDepth;  // A14: this view's water layer (v1.63 formats); the main view's are also  [V]
                                      // FrameResources::waterVis / waterDepth
    TextureRef gbuffer;            // RG32_UINT (GBuffer.hlsli)                              [M]
    TextureRef materialWord;       // R32_UINT: material 16 | metallic 8 | layer roughness 8 [M]
                                   // (MaterialInternal.hlsli mPackMaterialWord)
    TextureRef shadowVisibility;   // R32_UINT, 4 light slots x 8 bit (7.3)                 [S]
                                   // (in a frame with glass shadow casters - shadow.vsm.translucent_tint - twice the
                                   //  view's height: the rows below the view's are what they let through at each
                                   //  pixel, slot 0 x its luminance; ShadowVisibility.hlsli shadowSunTintChroma)
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
    TextureRef fogVolume;          // a planar reflection view's own fog volume (invalid: none; the main view's is    [S]
                                   // FrameResources::fogVolume)
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
    // v1.80 (render B, M's GI lookup moved out of shading): RGBA16F W x H of the main view, written by R's r.gi.screen at
    // the end of globalIllumination: rgb = giCacheIrradianceScreen at M's surface point (camera + mPixelRay x linear z, the
    // normal turned towards the viewer) x g_exposure; a = 1 where the cache has the value, 0 where M uses the probes.
    // Front faces only (M keeps its own lookup for Foliage back faces). Invalid = M looks the cache up itself.  [R]
    // cloud/render-fixes: after r.gi.screen.filter (GiScreenFilter.hlsl, gi.screen_filter_cells), the edge-preserving
    // spatial filter over the cache cells' blotches; a pixel without its own value takes its surface neighbours' (a = 1);
    // the main view's rgb carries the screen probes' near occlusion too (E x occlusion x exposure: M gathers the probes
    // only for the K path and Foliage's back side).
    TextureRef giIrradiance;
    // lumen.short_range_ao (A, for R's gi.lumen; Passes/GI/LumenShortRangeAO.hlsli): RGBA16F W x H of the main view,
    // xyz = world bent normal x AO, a = accumulated frames + 1 (0: no surface). Invalid = off. Readers use
    // lumenShortRangeAO / lumenAoMultibounce / lumenAoSpecular of that header.                                       [A]
    TextureRef shortRangeAO;
    // shading.mega_lights: the local lights' direct light after its filters (m.ml.spatial's result, RGBA16F W x H, x
    // exposure). For the gate's captures (layer "direct"); the shading kernels get it by their own constant. Invalid = off. [M]
    TextureRef localDirect;
    // shading.mega_lights_volume on a view other than the main one (planar reflection views; S records them with the view's
    // froxels): the view's froxel grid, the local lights' sampled visible fluence and direction moment, as
    // FrameResources::localFluence / localMoment are the main view's. Invalid = off.                                  [A]
    TextureRef localFluence, localMoment;
    // gi.lumen (v1.93, LumenGather.cpp): RGBA16F W x H of the main view, the mean incident radiance x g_exposure of the
    // pixel's rough specular lobe from the screen probes (rgb; a = the pixel filter's fast-update amount). From roughness
    // 0.6 the lobe fades to irradiance / pi, which it is from 0.8 (gi.lumen_max_roughness_rough_specular). The reader
    // multiplies its specular albedo. Invalid without gi.lumen.  [R]
    TextureRef giRoughSpecular;
    // gi.lumen, scenes with Foliage materials: RGBA16F W x H, the irradiance x g_exposure arriving on the BACK of a
    // Foliage pixel (the screen probes read at the reversed normal, with the diffuse irradiance's pixel history) - what
    // its diffuse transmission lights the viewer's side with. 0 on other pixels. Invalid: no Foliage material.      [R]
    TextureRef giBackfaceIrradiance;
    // The previous frame's scene colour for screen-space traces (shading::upscalePreviousColor, Passes/Shading/Upscale.cpp:
    // M's temporal upscale history of the frame before): RGBA16F at the OUTPUT resolution (FrameContext::upscale
    // outputWidth x outputHeight), rgb = linear radiance x the PREVIOUS frame's exposure (x FrameContext::upscale
    // exposureRatio = the same radiance at this frame's exposure), no jitter; a point's place in it is its projection by
    // FrameContext::upscale.prevViewProj. Invalid = no history: the first frame, a reset, or a frame that renders at its
    // output resolution (no upscale) - readers then skip their screen traces.  [M; added by S2 on the coordinator's decision]
    // Its size is the texture's own (RenderGraph::desc): the output's with output.screen_trace_source = 1 (the upscale's
    // history), the view's with 0 (the scene colour before the upscale).
    TextureRef prevSceneColor;
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
    // The temporal upscale's output (output resolution, RGBA16F: rgb = linear radiance x exposure before the post chain's
    // encoding, a = history weight; Upscale.cpp), for captures of the upscaled image; invalid when the frame renders at
    // its output resolution.                                                                  [M]
    TextureRef upscaled;
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
    // v1.61: 0 = coverage-layer see-through records (small surfaces: B8 fluid); 1 = the water layer (wide surfaces: B7
    // ocean and lakes; FrameResources::waterVis / waterDepth, one sample per pixel over band A).
    uint32_t layer = 0;
};
constexpr uint32_t kMaxTriangleStreams = 63;  // slot 63 is the view-grid ocean's (v1.73, COV_OCEAN_ID)  // vis id slot bits (CoverageTiles.hlsli COV_STREAM_ID)

// surface_cache.mesh_cards (Passes/SurfaceCache; unx/refl/SurfaceCacheCards.h): the mesh-card surface cache of this frame
// after its update. A ray hit reads it with clReadCards(mcFrame(frame SRV), scene instance, position, normal, ...)
// (Passes/SurfaceCache/CardLighting.hlsli); a pass that does declares every member (declareSurfaceCacheCards, Frame.h).
struct SurfaceCacheCardRefs
{
    BufferRef frame;                                                 // raw: the card frame (CardLayout.hlsli mcFrame)
    BufferRef instanceMap, meshCards, cards, cardPages, pageTable;   // records
    TextureRef depth, albedo, normal, emissive;                      // the cards' geometry atlases
    TextureRef direct, indirect, final;                              // their lighting
    // Written by the readers of the cards' high levels (CardLighting.hlsli clReadCardsHiRes; the card frame names
    // them): the feedback table and the pages' last-used words. Invalid: surface_cache.feedback off.
    BufferRef feedback, lastUsed;
    // The indirect light of hits without cards (Passes/GI/LumenHitIndirect.hlsli; the card frame's words 22..27 name
    // them): a translucency volume - the previous frame's until the final gather publishes this frame's - and, from the
    // final gather on, the radiance cache's irradiance probes with the list its hits ask for probes through.
    TextureRef hitVolumeAmbient, hitVolumeDirectional;
    TextureRef hitRcIndirection, hitRcIrradiance, hitRcDepth;
    BufferRef hitRcMarks;
    bool valid() const { return frame.valid(); }
};

// View-independent products of the current frame. Persistent state (VSM pool, GI cache, TLAS) is imported into the
// graph each frame by its owner.
// atmosphere.fog: the fog volume's grid and the medium (Passes/Atmosphere/FogVolume.hlsli, Fog.hlsli).
struct FogView
{
    bool on = false;
    uint32_t gridX = 0, gridY = 0, gridZ = 0, cellPx = 0;
    uint32_t farSlices = 0;        // slices past farM, to farEndM (the closed-form fog)
    float farEndM = 0;
    float farM = 0, k = 0, b = 0;  // slice(depth) = log2(depth k + 1) b
    float density = 0, falloff = 0, height = 0, g = 0, start = 0;  // extinction (1/m) at height, its halving per metre, m, HG g, m
    float density2 = 0, falloff2 = 0, height2 = 0;                 // the second layer (FogDesc::density2, or the rain's veil; 0: none)
    bool rainVeil = false;         // the second layer is the rain's (atmosphere.fog.rain_veil)
    bool farSkyLight = false;      // atmosphere.fog.far_sky_light (FogIntegrate.hlsl: the far slices' ambient light is the sky's)
    float albedo[3] = { 1, 1, 1 };
    float skyAmount = 0, historyWeight = 0.9f, shadowTexelsPerCell = 1;
    bool indirect = true;
    bool onRays = false;           // reflection rays take the fog along them (FogVolume.hlsli fogOverRay)
    uint32_t volumes = 0;          // the frame's local fog volumes that take effect (FrameContext::fogVolumes)
    bool cells = false;            // the cells hold a medium (the height fog or local volumes): s.fog.scatter runs, with its
                                   // shadow pages. false with on: the volume is there for the cloud layer alone
    bool airOrder = false;         // atmosphere.fog.air_order (FogIntegrate.hlsl: the volume's media ordered against the air's)
    bool onGiRays = false;         // atmosphere.fog.on_gi_rays (FogVolume.hlsli fogOverGiRay: the screen probes' rays)
    bool sunThroughFog = false;    // atmosphere.fog.sun_through_fog (FogVolume.hlsli: the thick-fog term)
    uint32_t cloudSteps = 0;       // atmosphere.clouds.veil: the frame's cloud layer in front of surfaces, marched along the
                                   // volume's columns in this many stretches (FogIntegrate.hlsl; 0: none)
    // the density's variation (FogVolume.hlsli fogDensityScale): its share of the density, its largest features (m), the
    // drift's speed with no wind (m/s) and the wind's share of the drift
    float noiseAmount = 0, noiseScale = 20, noiseDrift = 0.3f, noiseWind = 1;
};

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
    // V's skinned-instance bounds of this frame (prepareCullScene, v.cull.skinBounds): StructuredBuffer<float4> world     [V]
    // spheres, 2 per skin slot (current, previous palette and transform; radius < 0 = unbounded), the slots' instance
    // indices (SRV of a StructuredBuffer<uint>) and the slot count. S's page cache invalidates their shadow footprint.
    BufferRef skinBounds;
    uint32_t skinInstancesSrv = UINT32_MAX, skinCount = 0;
    // v1.45 (B2, COVERAGE 12.4 structure 2): raw SRV of 1 bit per scene light, set when the light's revision has held for  [M]
    // >= 8 frames and raytracing.emitters is on. M leaves those area lights' LTC specular to R's reflection paths in the
    // main view (G/M rays see the emitters; the K path's maps carry the GI cache's emitter texels) and shades it by LTC in
    // planar views (their cache reads are the texels alone); R's ray hits shade the lights by their NEE sample and read
    // the texels without the emitter texels. R excludes the unset ones from direct emitter hits, so no light is counted
    // twice or missed.
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
    // The hair density volume (E's Passes/Hair/HairDensity.hlsli; invalid = none): per body rho, the fibres a ray crosses
    // per metre, in cells (R16_FLOAT) and in means over 4^3 cells, and the parameters (raw: the bodies' boxes and the two
    // textures' SRVs - hairFibreCount takes this buffer's SRV alone; declare the three to read it). For M's hair records
    // and the hair's shadow on other surfaces (Passes/Hair/HairShadow.hlsl). hairOrigin: the world position the boxes are
    // relative to (the main view's camera) - a reader in another view adds its camera's offset from it.  [E]
    TextureRef hairDensity, hairDensityCoarse;
    BufferRef hairDensityParams;
    float3 hairOrigin{};
    // A3 FX particle lights (v1.81, render B's request): the whole scene light buffer (gpu::Light, stride 80; the FX tail at
    // [lightCount, lightCount + F)) and the count word (StructuredBuffer<uint>, element 0 = F), imported once per frame by
    // core so the graph orders the FX writer (Uav) before S, R and M (Srv). Invalid when the scene has no FX light tail
    // (GpuScene::fxLightRange().capacity == 0).  [core]
    BufferRef fxLights, fxLightCount;
    // GPU triangle streams of this frame (W, before V: tracks::waterGeometry; V's coverage layer, v1.60).  [W]
    std::vector<TriangleStream> triangleStreams;
    // Water layer (v1.61, main view; V draws the streams of layer 1 over a copy of band A's depth, band A stays): the nearest
    // water surface in front of band A per pixel - waterVis R32_UINT (COV_STREAM_ID | slot | triangle, VIS_NONE = no
    // water), waterDepth R32_FLOAT linear view depth (+inf = no water). Invalid when the frame has no water stream.  [V]
    TextureRef waterVis, waterDepth;
    // v1.73 (B7, W's view grid; W fills them in waterGeometry, before V): the sea per pixel of the main view -
    // oceanDepth R32_FLOAT linear view depth (+inf = no sea), waterSurface RGBA32_FLOAT (rest position x0.xz, depth,
    // marker: W's format). V merges oceanDepth into the water layer (COV_OCEAN_ID; band A rejects what lies behind).
    TextureRef oceanDepth, waterSurface;  // [W]
    // v1.77 (W stage 2, FEATURES_GAME 1.9; render A calls it from band A shading): the sun-space water map of W's
    // triangle streams, orthographic along the sun - waterSunDepth D32 (1 = nearest the sun, 0 = no water),
    // waterSunNormal RG32F octahedral normal (about +y), waterSunMedium RGBA16F (1 m transmittance RGB, IOR), and
    // waterSunConstants (raw: word 0 = valid). Passes/Water/WaterLight.hlsli waterSunLight reads them. Invalid = no water.
    TextureRef waterSunDepth, waterSunNormal, waterSunMedium;  // [W]
    TextureRef waterSunCaustics;  // R32_UINT array, 5 slices (WaterLight.hlsli waterCausticFactor; waterSunLight's causticsSrv)  [W]
    BufferRef waterSunConstants;                              // [W]
    // 14.1b (L2b, v1.92): the emissive quadtree lights image (lights::emissiveLights, Passes/Lights/EmissiveLights.hlsli;
    // invalid: shading.emissive_area_lights off or no converted emitter). Published by the first caller of the frame (M's
    // shading or R's GI hit); R's emissiveLightsConverted rule reads it from here.
    BufferRef emissiveLights;                                  // [A/Lights]
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
    BufferRef vsmTileLit;          // L3 (14.3-2): per 8x8 main-view tile, the froxel-list entries lit over   [S]
                                   // every pixel (VsmCls.hlsli VsmClsTile; invalid = classification off)
    BufferRef tlasStatic, tlasDynamic;  // acceleration structures                          [R]
    BufferRef giCache;             // world radiance cache                                  [R]
    TextureRef screenTraceHzb;     // the shared screen trace's depth pyramid (ScreenTrace.hlsli sctLevelOrigin) of the main  [R]
                                   // view, published by tracks::screenTraceInputs before GI; invalid: not asked for
    BufferRef surfaceCache;        // the surface cache (Passes/SurfaceCache, SURFACE_CACHE_INTERFACE_KO.md): this frame's  [R]
                                   // import, published by tracks::surfaceCache before GI; ray hits mark and read it as a
                                   // UAV (DispatchRays passes: UavGraphics). Invalid: off, or before its first frame
    SurfaceCacheCardRefs cards;    // the mesh-card surface cache after this frame's update, published by           [R]
                                   // tracks::surfaceCache before GI; invalid: surface_cache.mesh_cards off or no card yet
    BufferRef giAccumulator;       // hit direct-light accumulator pool (GiAccPool.hlsli;   [R]
                                   // invalid: gi.hit_accumulator_pool off). Readers after
                                   // globalIllumination: SrvCompute (giAccPoolRead)
    // atmosphere.fog (S; Passes/Atmosphere/FogVolume.hlsli): the main view's fog after s.fog.integrate - fogVolume
    // (Texture3D RGBA16F: in-scattered radiance x the view's exposure and transmittance to each slice's far face) and the grid and
    // medium (fog; on = false: no fog this frame). Kernels read it through the frame constants (g_fog) inside the air
    // lookups; their passes declare it with declareFog (Frame.h). With the frame's cloud layer (atmosphere.clouds.veil)
    // the volume is there without fog too and holds the cloud in front of surfaces.                                 [S]
    TextureRef fogVolume;
    FogView fog;
    TextureRef fogSecondary[4];    // the planar reflection views' fog volumes of this frame (declareFog declares them too)
    TextureRef fogDebug;  // atmosphere.fog.debug_view: the fog alone per pixel (FogDebug.hlsl; the gate's layer "fog")
    // shading.mega_lights_volume (A; Passes/Atmosphere/MegaLightsVolume.hlsl): the main view's froxel grid, RGBA16F, the
    // local lights' sampled visible fluence (rgb x exposure) and its luminance-weighted direction moment (xyz): lit
    // particles read them (FxLayerSetup.hlsl). Invalid = off.                                                         [A]
    TextureRef localFluence, localMoment;
    // lumen.radiance_cache (A, for R's gi.lumen; Passes/GI/LumenRadianceCache.hlsli, unx/gi/LumenRadianceCache.h): this
    // frame's far-field probe cache after its update. Readers: lrcParams(lumenRcParams), lrcCoverageChecked, lrcSample
    // with the indirection (Texture3D R32_UINT) and the atlas (R11G11B10F) as SRVs. Invalid = off.                   [A]
    TextureRef lumenRcIndirection, lumenRcAtlas, lumenRcDepth;
    uint32_t lumenRcParams = 0xFFFFFFFFu;
    // gi.lumen_cap_snap_exposure (LgMeter.hlsl): on a snap frame (first frames, a cut, a restore: the frame's exposure
    // was not metered on what it shows) a raw buffer whose first float is c = the exposure metered on the frame's own
    // traces / the frame's exposure; R's reflection filters take their caps and tone-mapped means in g_exposure x c.
    // Invalid on every other frame.                                                                                 [R]
    BufferRef lumenCapReference;
    // lumen.translucency_volume (Passes/GI/LumenTranslucencyVolume.hlsli, unx/gi/LumenTranslucencyVolume.h): the main
    // view's indirect light for what is not an opaque surface - air and fog, particles, water, glass. Readers pass
    // translucencyGiParams (a raw SRV; 0xFFFFFFFF = off) to ltvIrradiance / ltvRadiance and declare the two volume
    // textures as shader resources. Published by the final gather (tracks::globalIllumination).                      [R]
    TextureRef translucencyGiAmbient, translucencyGiDirectional;
    uint32_t translucencyGiParams = 0xFFFFFFFFu;
    // The same volume as the PREVIOUS frame left it, for passes recorded before this frame's final gather (S's fog:
    // the air volume is built before GI). Published by tracks::translucencyVolumePrevious at the frame's start;
    // 0xFFFFFFFF / invalid: no volume last frame (the first frame, a cut, the module off).                        [R]
    TextureRef translucencyGiPrevAmbient, translucencyGiPrevDirectional;
    uint32_t translucencyGiPrevParams = 0xFFFFFFFFu;
};
} // namespace unx::render
