#pragma once
// The surface cache on mesh cards (surface_cache.mesh_cards): the renderer's own implementation of Unreal's Lumen scene
// (LumenScene.cpp, LumenSceneRendering.cpp, LumenSceneCardCapture.cpp, LumenSceneLighting.cpp). Every rigid instance in
// range keeps a few axis-aligned cards - orthographic captures of its surfaces into atlases (depth, albedo, normal,
// emission) - at a resolution set by its distance from the camera, whatever the camera looks at; the cards' texels are
// lit (direct light, radiosity) on a budget per frame, and every ray hit of the frame (screen-probe GI, reflections,
// the far radiance cache, refractions) reads the lighting of the surface it met. Nothing in it is fed by the view, so
// a camera cut or a turn finds the lighting of what it now sees already there.
//
// One instance per FrameRenderer (track state "R.cards"). Per frame, before the GI tracks:
//   the card scene follows the GPU scene (instances added, moved, hidden, re-materialed; cards generated off the frame
//   and kept on disk: MeshCardCache), each card asks for its resolution (MeshCardScene::update), then per update round
//   r.card.frame -> r.card.capture (raster, the frame's new and refreshed pages into the capture atlas)
//   -> r.card.resample (re-allocated cards keep their lighting) -> r.card.upload (records) -> r.card.copy (atlases)
//   -> CardLighting::recordLighting (selection, direct light, radiosity, final lighting).
// The capture draws a page's instance from its source triangles (CardCapture.ms/.ps.hlsl), or - with
// surface_cache.mesh_cards_capture_clusters - from the cluster hierarchy's cut at the page's texel size through V's
// raster service (FrameServices::rasterizeDepth: a page is a view of a request; r.card.vcapture draws the depth and the
// material images in one run - CardCaptureCluster.ps.hlsl).
// One round a frame; surface_cache.mesh_cards_load_rounds while a level loads, so the cache is whole within a few frames
// of a load instead of a hundred.
// Feedback (surface_cache.feedback; Unreal's LumenSurfaceCacheFeedback): the frame's readers of the cards' high levels
// (reflections, the radiosity) report the page and level each hit wanted into a table (CardLighting.hlsli clFeedback);
// at the start of the next frame the table is copied for the CPU and emptied (r.card.feedback.readback / .clear), and
// the frame that finds the copy complete - framesInFlight later - hands it to MeshCardScene::setFeedback, which maps
// and captures those pages above the cards' resident levels and lets the ones no hit asks for any more leave.
#include "unx/refl/CardLighting.h"
#include "unx/refl/MeshCardCache.h"
#include "unx/refl/MeshCardScene.h"
#include "unx/render/Frame.h"
#include "unx/rt/RayScene.h"

#include <memory>

namespace unx::render::refl
{
struct SurfaceCacheCardSettings  // Config/quality/surface_cache.toml
{
    bool enabled = false;
    McSettings cards;
    bool direct = true, radiosity = true, shadowRaysOpaque = false;
    float radiosityCap = 40.0f, radiosityFrames = 4.0f;
    float radiositySkipBackFace = 0.05f, radiositySkipTwoSided = 0.01f;  // metres; 0: the radiosity rays are not re-shot
    float radiosityMinTraceDistance = 0.10f;
    uint32_t directFactor = 32, radiosityFactor = 64;
    float depthBias = 0.10f;
    uint32_t loadRounds = 8;          // update rounds a frame while a level loads
    uint32_t loadLightingRounds = 96; // rounds after the last card of a load was captured (direct light of every page,
                                      // then the radiosity's bounces)
    bool captureClusters = false;     // surface_cache.mesh_cards_capture_clusters: the captures are drawn from the cluster
                                      // hierarchy's cut through V's raster service (CardCaptureCluster.ps.hlsl)
    float feedbackResLevelBias = -0.5f;  // surface_cache.feedback_res_level_bias (cards.feedback: the switch)
    bool lightingFeedback = true;        // surface_cache.lighting_feedback
    bool emissiveLightSources = true;    // surface_cache.mesh_cards_emissive_light_sources (MeshCardScene::addInstance)
    CardHitRules hitRules;               // lumen.skylight_leaking*, lumen.radiance_cache_far_field,
                                         // reflection.lumen_distant_screen_trace* (the card frame carries them to the hits)
    std::string cacheDirectory;       // mesh card files; "" = the default directory, "none" = no disk cache
    static SurfaceCacheCardSettings fromQuality(const QualityConfig& q);
};

class SurfaceCacheCards
{
public:
    static SurfaceCacheCards& get(FramePassContext& fc);
    static SurfaceCacheCards* find(TrackState& state);
    SurfaceCacheCards();
    ~SurfaceCacheCards();
    SurfaceCacheCards(const SurfaceCacheCards&) = delete;
    SurfaceCacheCards& operator=(const SurfaceCacheCards&) = delete;

    // The frame's card work; publishes FrameResources::cards (invalid when surface_cache.mesh_cards is off or no card
    // exists yet). Once per frame, after the acceleration structures and the atmosphere tables, before GI.
    void record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays);
    // Constant sky radiance (nits) and sun illuminance (lux) when S's atmosphere tables are absent (tests).
    void setConstantSky(float3 radiance, float3 sunIlluminance);
    // Every resource a reader of FrameResources::cards touches (clReadCards), as shader resources of 'use'.
    void declareRead(const FramePassContext& fc, PassBuilder& b, Use use) const;

    const McStats& stats() const;
    // A level is loading: cards are still generated, captured or lit for the first time (a host may hold its load
    // screen until it is false).
    bool loading() const;
    // Blocks until the generation of every mesh's cards asked for so far has finished (tools, tests).
    void waitForGeneration();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace unx::render::refl
