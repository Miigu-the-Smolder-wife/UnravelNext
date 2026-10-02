// Volumetric clouds in the frame (CloudSystem.h).
#include "CloudSystem.h"
#include "CloudGpu.h"
#include "SResources.h"

#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace unx::render::atmosphere
{
namespace
{
using namespace s_detail;
const char* const kCloudKey = "s.clouds";
constexpr uint32_t kRingSlots = 4, kRecordBytes = 256, kMapTexels = 256, kBandRows = 64;
constexpr uint32_t kDomeWidth = 256, kDomeHeight = 96;  // the sky dome (CloudCommon.hlsli cloudDomeUv): 1.4 deg azimuth
constexpr uint32_t kDomeBandRows = 8, kDomeBands = kDomeHeight / kDomeBandRows;  // the dome is refreshed in bands of rows
constexpr float kMapHalfExtent = 16000;

struct CloudState
{
    clouds::CloudLayer layer;
    bool set = false, setByTest = false;
    Device* device = nullptr;
    clouds::CloudTextures noise;
    bool noiseReady = false;
    uint32_t noiseSeed = 0;
    // radiance, distance: this frame's layer and last frame's (the march rebuilds 3 texels of 4 from it), by parity
    ComPtr<ID3D12Resource> radiance[2], distance[2], map, ring, stats, zeros, statsReadback, record, dome;
    uint32_t radianceSrv[2] = {}, distanceSrv[2] = {}, mapSrv = 0, recordSrv = 0, domeSrv = 0;
    uint32_t parity = 0;
    bool history = false, domeFilled = false;  // last frame's layer is this view's; the dome has been filled once
    bool flashBefore = false;                  // last frame had a lightning flash (its light must leave every texel)
    // The dome's refresh by what moves in it (atmosphere.clouds.dome_adaptive; domeDue): per band, how far its picture has
    // drifted since the band was last marched, in texels' shares of the allowed drift (>= 1: the band is due); the
    // camera, the time, the sun and the layer the drift is counted from.
    float domeDrift[kDomeBands] = {};
    double domeCamera[3] = { 0, 0, 0 }, domeTime = 0;
    float3 domeSun{};
    float domeSunLight = 0;
    uint64_t domeLayer = 0;
    uint8_t* ringMapped = nullptr;
    uint32_t width = 0, height = 0;
    uint64_t frames = 0;
    ~CloudState()
    {
        if (!device) return;
        DescriptorHeaps& h = device->descriptors();
        for (uint32_t i : { radianceSrv[0], radianceSrv[1], distanceSrv[0], distanceSrv[1], mapSrv, recordSrv, domeSrv })
            if (i) h.freeResource(i);
        if (noiseReady) clouds::releaseTextures(*device, noise);
        for (ComPtr<ID3D12Resource>* r : { std::addressof(radiance[0]), std::addressof(radiance[1]), std::addressof(distance[0]), std::addressof(distance[1]),
                                           std::addressof(map), std::addressof(ring), std::addressof(stats), std::addressof(zeros), std::addressof(statsReadback),
                                           std::addressof(record), std::addressof(dome) })
            if (*r) device->deferRelease(*r);
    }
};

TextureDesc textureDesc(const char* name, uint32_t w, uint32_t h, DXGI_FORMAT format)
{
    TextureDesc d{ name, w, h, 1, 1, format };
    return d;
}

uint32_t textureSrv(Device& device, ID3D12Resource* r, DXGI_FORMAT format)
{
    const uint32_t index = device.descriptors().allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = format;
    sd.Texture2D.MipLevels = 1;
    device.d3d()->CreateShaderResourceView(r, &sd, device.descriptors().resourceCpu(index));
    return index;
}

bool enabled(const CloudState& s) { return s.set && (s.layer.coverage > 0 || s.layer.cirrusCoverage > 0); }

// What the dome's picture is made from besides the wind's drift, the camera and the sun: a change of any of it shows in
// every texel at once (the whole dome is marched again).
uint64_t domeLayerKey(const clouds::CloudLayer& l, uint32_t sunWord, float powder)
{
    const double v[] = { l.baseAltitude, l.topAltitude, l.coverage, l.sigmaMax, l.albedo, l.detailStrength, l.g0, l.g1, l.lobeBlend, l.shapePeriod, l.detailPeriod,
                         l.weatherPeriod, l.windX, l.windZ, l.cirrusCoverage, l.cirrusAltitude, l.cirrusOpticalDepth, l.cirrusPeriod, l.cirrusWindX, l.cirrusWindZ,
                         (double)sunWord, powder };
    uint64_t h = 1469598103934665603ull;  // (FNV-1a over the values' bytes)
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(v);
    for (size_t i = 0; i < sizeof v; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

// Distance (m) from a point at altitude 'from' along a direction of elevation e (its sine) to the sphere of altitude 'to'
// above the planet (radius R); a large value when the direction never meets it.
double domeShellDistance(double R, double from, double to, double sinE)
{
    const double r = R + from, rs = R + to, b = r * sinE, c = r * r - rs * rs;
    const double disc = b * b - c;
    if (disc < 0) return 1e12;
    const double root = std::sqrt(disc);
    const double first = -b - root, second = -b + root;  // (the nearer root where the point is outside the sphere)
    return first > 0 ? first : (second > 0 ? second : 1e12);
}

// atmosphere.clouds.dome_adaptive: which bands of the dome this frame marches. The dome is the cloud seen from the camera
// in every direction; its picture moves when the cloud drifts with the wind, when the camera moves, and when the sun's
// direction or light changes. A band's picture has drifted by the relative motion over the distance to the nearest cloud
// in its directions (the layer's nearer boundary and the cirrus sheet, whichever turns faster): the turn square to the
// line of sight in the dome's azimuth steps (1.4 deg), the turn in elevation in the band's rows; the sun's turn counts
// in azimuth steps, a change of its light as a share of maxLight. A band is marched when its drift reaches 'allowed' texels, or maxAge seconds after its last march
// (what nothing here follows - the air's medium - still reaches the dome). A still sky under a slow sun then costs a band
// every few seconds; under a fast one the bands overhead are marched every frame and the horizon's as their slower
// turn asks.
// Returns the bands due as bits; 'whole' marches all of them and starts the count anew.
uint32_t domeDue(CloudState& s, const FramePassContext& fc, const ViewDesc& view, float3 sunDir, float sunLight, double bottomRadius, float allowed, float maxAge, bool whole)
{
    const float3 origin = fc.scene.originOffset();
    const double camera[3] = { (double)view.position.x + origin.x, (double)view.position.y + origin.y, (double)view.position.z + origin.z };
    const double elapsed = std::abs(fc.frame.time - s.domeTime);
    // the cloud's motion relative to the camera since the last frame: the wind over the World's time (the field's
    // advection, CloudModel.cpp offsetsFor) less the camera's own step
    const double step[3] = { camera[0] - s.domeCamera[0], camera[1] - s.domeCamera[1], camera[2] - s.domeCamera[2] };
    auto relative = [&](double windX, double windZ, double& across, double& up) {
        const double dx = windX * elapsed - step[0], dz = windZ * elapsed - step[2];
        across = std::sqrt(dx * dx + dz * dz);
        up = std::abs(step[1]);
    };
    double layerAcross = 0, layerUp = 0, cirrusAcross = 0, cirrusUp = 0;
    relative(s.layer.windX, s.layer.windZ, layerAcross, layerUp);
    relative(s.layer.cirrusWindX, s.layer.cirrusWindZ, cirrusAcross, cirrusUp);
    const double sunTurn = std::acos(std::clamp((double)dot(sunDir, s.domeSun), -1.0, 1.0));
    constexpr double kMaxLight = 0.03;  // a band is due when the sun's light has changed by this share
    const double lightChange = s.domeSunLight > 0 ? std::abs((double)sunLight - s.domeSunLight) / s.domeSunLight : (sunLight > 0 ? 1.0 : 0.0);
    // (the camera's altitude above the planet: the world's origin offset set to the camera, the point at 0)
    const double here[3] = { 0, 0, 0 };
    const double altitude = clouds::altitudeOf(clouds::offsetsFor(s.layer, camera, 0.0), bottomRadius, here);
    s.domeCamera[0] = camera[0], s.domeCamera[1] = camera[1], s.domeCamera[2] = camera[2];
    s.domeTime = fc.frame.time;
    s.domeSun = sunDir;
    s.domeSunLight = sunLight;
    if (whole)
    {
        for (float& d : s.domeDrift) d = 0;
        return (1u << kDomeBands) - 1u;
    }
    constexpr double kPi = 3.14159265358979, kE0 = -0.17453293;  // (CloudCommon.hlsli CLOUD_DOME_E0)
    constexpr double kNearest = 200.0;                           // m: the camera inside the cloud turns it no faster than this
    const double azimuthTexel = 2 * kPi / kDomeWidth;
    uint32_t due = 0;
    for (uint32_t band = 0; band < kDomeBands; ++band)
    {
        // the band's elevations (v = row / height, e = e0 + v^2 (pi / 2 - e0)) and its rows' step in elevation
        const double v0 = (double)(band * kDomeBandRows) / kDomeHeight, v1 = (double)((band + 1) * kDomeBandRows) / kDomeHeight;
        const double e0 = kE0 + v0 * v0 * (kPi / 2 - kE0), e1 = kE0 + v1 * v1 * (kPi / 2 - kE0);
        const double elevationTexel = (e1 - e0) / kDomeBandRows;
        double drift = 0;
        for (double e : { e0, e1 })
        {
            const double sinE = std::sin(e), cosE = std::cos(e);
            auto turned = [&](double distance, double across, double up) {
                const double d = std::max(distance, kNearest);
                // The picture's turn square to the line of sight against the dome's azimuth step (a true angle: the
                // azimuth texels narrow toward the zenith, the lag that shows does not), and its turn in elevation in
                // the band's own rows (fine toward the horizon, where the cloud's features are).
                return std::max(across / d / azimuthTexel, (across * std::abs(sinE) + up * std::abs(cosE)) / d / elevationTexel);
            };
            if (s.layer.coverage > 0)
            {
                // the layer's nearer boundary along this elevation (inside the layer: the cloud is at the camera)
                const bool inside = altitude >= s.layer.baseAltitude && altitude <= s.layer.topAltitude;
                const double d = inside ? 0.0 : domeShellDistance(bottomRadius, altitude, altitude < s.layer.baseAltitude ? s.layer.baseAltitude : s.layer.topAltitude, sinE);
                drift = std::max(drift, turned(d, layerAcross, layerUp));
            }
            if (s.layer.cirrusCoverage > 0) drift = std::max(drift, turned(domeShellDistance(bottomRadius, altitude, s.layer.cirrusAltitude, sinE), cirrusAcross, cirrusUp));
        }
        drift = drift / std::max((double)allowed, 1e-3) + sunTurn / azimuthTexel / std::max((double)allowed, 1e-3) + lightChange / kMaxLight;
        if (maxAge > 0) drift += std::abs((double)fc.frame.deltaTime) / maxAge;
        s.domeDrift[band] = (float)std::min((double)s.domeDrift[band] + drift, 1e6);
        if (s.domeDrift[band] >= 1.0f)
        {
            due |= 1u << band;
            s.domeDrift[band] = 0;
        }
    }
    return due;
}
} // namespace

uint32_t cloudSunWord(const QualityConfig& q)
{
    const uint32_t steps = q.has("atmosphere.clouds.sun_steps") ? (uint32_t)std::min<int64_t>(std::max<int64_t>(q.integer("atmosphere.clouds.sun_steps"), 0), 64) : 0u;
    const bool filtered = !q.has("atmosphere.clouds.filtered_steps") || q.boolean("atmosphere.clouds.filtered_steps");
    const bool ground = !q.has("atmosphere.clouds.ground_light") || q.boolean("atmosphere.clouds.ground_light");
    return steps | (filtered ? 0x100u : 0u) | (ground ? 0x200u : 0u);
}

namespace
{

// This frame's layer: FrameContext::clouds (the host's UnxFrameSetClouds, v1.77) when it has coverage, else the one a gate
// or test set (setCloudLayer).
void takeFrameLayer(FramePassContext& fc, CloudState& s)
{
    const CloudLayerDesc& d = fc.frame.clouds;
    if (d.coverage <= 0 && d.cirrusCoverage <= 0 && s.setByTest) return;
    s.layer.coverage = d.coverage, s.layer.baseAltitude = d.baseAltitude, s.layer.topAltitude = d.topAltitude;
    s.layer.sigmaMax = d.sigmaMax, s.layer.albedo = d.albedo, s.layer.windX = d.windX, s.layer.windZ = d.windZ;
    s.layer.cirrusCoverage = d.cirrusCoverage, s.layer.cirrusAltitude = d.cirrusAltitude, s.layer.cirrusOpticalDepth = d.cirrusOpticalDepth;
    s.layer.cirrusWindX = d.cirrusWindX, s.layer.cirrusWindZ = d.cirrusWindZ;
    if (d.cirrusCoverage > 0 && !(d.cirrusAltitude > 0 && d.cirrusOpticalDepth > 0 && d.cirrusCoverage <= 1))
        fail("FrameContext::clouds: the cirrus sheet's coverage in [0, 1], altitude and optical depth > 0");
    s.set = d.coverage > 0 || d.cirrusCoverage > 0;
    s.setByTest = false;
}
} // namespace

void setCloudLayer(TrackState& state, const clouds::CloudLayer& layer)
{
    CloudState& s = state.get<CloudState>(kCloudKey);
    s.layer = layer;
    s.set = true;
    s.setByTest = true;
}

void cloudsPrepare(FramePassContext& fc, uint32_t& recordSrv)
{
    CloudState& s = fc.state<CloudState>(kCloudKey);
    takeFrameLayer(fc, s);
    recordSrv = 0;
    if (!enabled(s)) return;
    Device& device = fc.device;
    s.device = &device;
    if (!s.noiseReady)
    {
        s.noise = clouds::uploadTextures(device, clouds::generateNoise(1));
        s.noiseReady = true;
    }
    const uint32_t w = (fc.frame.mainView.width + 3) / 4, h = (fc.frame.mainView.height + 3) / 4;
    if (w != s.width || h != s.height)
    {
        DescriptorHeaps& heaps = device.descriptors();
        for (uint32_t* i : { &s.radianceSrv[0], &s.radianceSrv[1], &s.distanceSrv[0], &s.distanceSrv[1] })
            if (*i) heaps.freeResource(*i), *i = 0;
        for (ComPtr<ID3D12Resource>* r : { std::addressof(s.radiance[0]), std::addressof(s.radiance[1]), std::addressof(s.distance[0]), std::addressof(s.distance[1]) })
            if (*r) device.deferRelease(*r);
        const auto T2 = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        for (int k = 0; k < 2; ++k)
        {
            s.radiance[k] = createTexture(device, L"S cloud layer", T2, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
            s.distance[k] = createTexture(device, L"S cloud distance", T2, w, h, 1, DXGI_FORMAT_R16_FLOAT, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
            s.radianceSrv[k] = textureSrv(device, s.radiance[k].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            s.distanceSrv[k] = textureSrv(device, s.distance[k].Get(), DXGI_FORMAT_R16_FLOAT);
        }
        s.width = w, s.height = h;
        s.history = false;
    }
    if (!s.map)
    {
        s.map = createTexture(device, L"S cloud sun map", D3D12_RESOURCE_DIMENSION_TEXTURE2D, kMapTexels * 2, kMapTexels, 1, DXGI_FORMAT_R32G32B32A32_UINT,
                              D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
        s.mapSrv = textureSrv(device, s.map.Get(), DXGI_FORMAT_R32G32B32A32_UINT);
        s.dome = createTexture(device, L"S cloud sky dome", D3D12_RESOURCE_DIMENSION_TEXTURE2D, kDomeWidth, kDomeHeight, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                               D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
        s.domeSrv = textureSrv(device, s.dome.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        s.ring = createBuffer(device, L"S cloud record ring", kRingSlots * kRecordBytes, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE none{ 0, 0 };
        check(s.ring->Map(0, &none, reinterpret_cast<void**>(&s.ringMapped)), "map cloud record ring");
        // The record every reader uses (a fixed SRV the atmosphere record names), refreshed from the ring each frame.
        s.record = createBuffer(device, L"S cloud record", kRecordBytes);
        s.recordSrv = device.descriptors().allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.Buffer.NumElements = kRecordBytes / 4;
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device.d3d()->CreateShaderResourceView(s.record.Get(), &sd, device.descriptors().resourceCpu(s.recordSrv));
        s.stats = createBuffer(device, L"S cloud stats", 256);
        s.zeros = createBuffer(device, L"S cloud stats zeros", 256, D3D12_HEAP_TYPE_UPLOAD);
        void* m = nullptr;
        check(s.zeros->Map(0, &none, &m), "map cloud zeros");
        std::memset(m, 0, 256);
        s.zeros->Unmap(0, nullptr);
    }
    recordSrv = s.recordSrv + 1;
}

void cloudsRecord(FramePassContext& fc, TextureRef transmittanceLut)
{
    CloudState& s = fc.state<CloudState>(kCloudKey);
    if (!enabled(s) || !s.radiance[0]) return;
    RenderGraph& g = fc.graph;
    const ViewDesc& mv = fc.frame.mainView;
    const scene::Scene* src = fc.scene.source();
    const float3 sunDir = normalize(src ? src->sun.direction : scene::Sun{}.direction);
    const double bottomRadius = (src ? src->atmosphere : scene::Atmosphere{}).bottomRadius;
    const float3 origin = fc.scene.originOffset();
    const double o[3] = { origin.x, origin.y, origin.z };
    const clouds::CloudOffsets offsets = clouds::offsetsFor(s.layer, o, fc.frame.time);
    const float sd[3] = { sunDir.x, sunDir.y, sunDir.z }, one[3] = { 1, 1, 1 };
    const float centre[3] = { mv.position.x, 0.5f * (s.layer.baseAltitude + s.layer.topAltitude), mv.position.z };
    clouds::CloudRecord rec = clouds::makeRecord(s.layer, offsets, s.noise, bottomRadius, sd, one, centre, kMapHalfExtent, kMapTexels);
    rec.shadow = s.mapSrv;
    // atmosphere.clouds.powder (CloudCommon.hlsli cloudMsSun; 0: the fitted octaves alone)
    rec.powder = fc.quality.has("atmosphere.clouds.powder") ? (float)std::clamp(fc.quality.number("atmosphere.clouds.powder"), 0.0, 1.0) : 0.0f;
    // The frame's lightning flash, in the renderer's space.
    const LightningDesc& flash = fc.frame.lightning;
    const bool flashNow = flash.intensity > 0;
    if (flashNow)
    {
        rec.flashPosition[0] = (float)(flash.position[0] - o[0]), rec.flashPosition[1] = (float)(flash.position[1] - o[1]);
        rec.flashPosition[2] = (float)(flash.position[2] - o[2]);
        rec.flashRadius = std::max(flash.radius, 1.0f);
        for (int k = 0; k < 3; ++k) rec.flashIntensity[k] = flash.intensity * std::max(flash.color[k], 0.0f);
    }
    // (a flash lights every texel now and leaves every texel the frame after: neither frame takes last frame's texels,
    //  and both refresh the whole dome - the escaping rays' sky shows the flash in its frame)
    const bool flashFrame = flashNow || s.flashBefore;
    s.flashBefore = flashNow;
    // this frame's layer (by parity); last frame's is the march's history when the view went on from it
    const uint32_t previousLayer = s.parity, layer = previousLayer ^ 1u;
    s.parity = layer;
    const QualityConfig& q = fc.quality;
    const bool temporal = !q.has("atmosphere.clouds.temporal") || q.boolean("atmosphere.clouds.temporal");
    // the sun path's marched steps per sample (CloudShadowCommon.hlsli cloudSunTauNear; 0: the whole path)
    // (| flags << 8: CloudMarch.hlsl P[3].w - the steps' mean density, the ground's light)
    const uint32_t sunSteps = cloudSunWord(q);
    const bool history = temporal && s.history && fc.frame.discontinuity == 0 && !flashFrame;
    s.history = true;
    rec.layerSrv = s.radianceSrv[layer], rec.distanceSrv = s.distanceSrv[layer], rec.skySrv = s.domeSrv;
    const uint32_t slot = (uint32_t)(s.frames++ % kRingSlots);
    std::memcpy(s.ringMapped + slot * kRecordBytes, &rec, sizeof rec);
    const uint32_t recordSrv = s.recordSrv;
    ID3D12Resource* ring = s.ring.Get();
    const BufferRef record = g.importBuffer(s.record.Get(), BufferDesc{ "S cloud record", kRecordBytes, 0 });

    const auto L = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    const TextureRef map = g.importTexture(s.map.Get(), textureDesc("S cloud sun map", kMapTexels * 2, kMapTexels, DXGI_FORMAT_R32G32B32A32_UINT), L);
    const TextureRef radiance = g.importTexture(s.radiance[layer].Get(), textureDesc("S cloud layer", s.width, s.height, DXGI_FORMAT_R16G16B16A16_FLOAT), L);
    const TextureRef distance = g.importTexture(s.distance[layer].Get(), textureDesc("S cloud distance", s.width, s.height, DXGI_FORMAT_R16_FLOAT), L);
    TextureRef previousRadiance, previousDistance;
    if (history)
    {
        previousRadiance = g.importTexture(s.radiance[previousLayer].Get(), textureDesc("S cloud layer (previous)", s.width, s.height, DXGI_FORMAT_R16G16B16A16_FLOAT), L);
        previousDistance = g.importTexture(s.distance[previousLayer].Get(), textureDesc("S cloud distance (previous)", s.width, s.height, DXGI_FORMAT_R16_FLOAT), L);
    }
    // this frame's texel of each 2 x 2 block (the order 0, 3, 1, 2: opposite corners in turn)
    static const uint32_t kBlockOrder[4] = { 0, 3, 1, 2 };
    const uint32_t blockTexel = kBlockOrder[s.frames % 4];
    // The dome, once it has been filled: the bands whose picture has moved (atmosphere.clouds.dome_adaptive), or 16 rows a
    // frame in turn (6 frames for the whole dome, whatever moves).
    const bool domeAdaptive = q.has("atmosphere.clouds.dome_adaptive") && q.boolean("atmosphere.clouds.dome_adaptive");
    const float domeAllowed = domeAdaptive ? (float)q.number("atmosphere.clouds.dome_drift_texels") : 0.5f;
    const float domeMaxAge = domeAdaptive ? (float)q.number("atmosphere.clouds.dome_max_age_s") : 0.0f;
    if (domeAdaptive && (!(domeAllowed >= 0.05f && domeAllowed <= 4) || !(domeMaxAge >= 0))) fail("atmosphere.clouds: dome_drift_texels in [0.05, 4], dome_max_age_s >= 0");
    const uint64_t domeLayer = domeLayerKey(s.layer, sunSteps, rec.powder);
    const bool domeWhole = !temporal || !s.domeFilled || fc.frame.discontinuity != 0 || flashFrame || (domeAdaptive && domeLayer != s.domeLayer);
    s.domeFilled = true;
    s.domeLayer = domeLayer;
    uint32_t domeBands = 0;  // bit b: rows [8 b, 8 b + 8) are marched this frame
    if (domeAdaptive)
    {
        const scene::Sun sun = src ? src->sun : scene::Sun{};
        const float sunLight = sun.illuminance * (0.2126f * sun.color.x + 0.7152f * sun.color.y + 0.0722f * sun.color.z);
        domeBands = domeDue(s, fc, mv, sunDir, sunLight, bottomRadius, domeAllowed, domeMaxAge, domeWhole);
    }
    else if (domeWhole) domeBands = (1u << kDomeBands) - 1u;
    else domeBands = 3u << (2u * (uint32_t)(s.frames % (kDomeBands / 2)));
    const TextureRef dome = g.importTexture(s.dome.Get(), textureDesc("S cloud sky dome", kDomeWidth, kDomeHeight, DXGI_FORMAT_R16G16B16A16_FLOAT), L);
    const BufferRef stats = g.importBuffer(s.stats.Get(), BufferDesc{ "S cloud stats", 256, 0 });
    ShaderLibrary& sh = fc.shaders;
    ID3D12Resource* zeros = s.zeros.Get();
    g.addPass("s.cloud.statsclear", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(stats, Use::CopyDst);
                  b.keep();
              },
              [stats, zeros](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(stats), 0, zeros, 0, 16); });
    g.addPass("s.cloud.record", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(record, Use::CopyDst);
                  b.keep();
              },
              [record, ring, slot](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(record), 0, ring, (uint64_t)slot * kRecordBytes, kRecordBytes); });
    ID3D12PipelineState* pm = sh.compute("Passes/Atmosphere/CloudShadow");
    g.addPass("s.cloud.map", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(record, Use::SrvCompute);
                  b.use(map, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[4] = { recordSrv, c.uav(map), 0, 0 };
                  c.cmd->SetPipelineState(pm);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(groups(kMapTexels, 8), groups(kMapTexels, 8), 1);
              });
    ID3D12PipelineState* pc = sh.compute("Passes/Atmosphere/CloudMarch");
    const D3D12_GPU_VIRTUAL_ADDRESS cb = fc.frameConstantsFor(mv);
    const uint32_t w = s.width, h = s.height;
    const TextureRef msTable = fc.resources.multiScatterLut;
    g.addPass("s.cloud.march", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(record, Use::SrvCompute);
                  b.use(map, Use::SrvCompute);
                  b.use(transmittanceLut, Use::SrvCompute);
                  b.use(msTable, Use::SrvCompute);
                  b.use(radiance, Use::UavCompute);
                  b.use(distance, Use::UavCompute);
                  b.use(stats, Use::UavCompute);
                  if (history)
                  {
                      b.use(previousRadiance, Use::SrvCompute);
                      b.use(previousDistance, Use::SrvCompute);
                  }
              },
              [=](PassContext& c) {
                  c.cmd->SetPipelineState(pc);
                  c.bindFrameConstants(cb);
                  // 64-row bands: each dispatch's worst time is bounded (the whole view in one dispatch could approach the
                  // TDR limit: 0.19 us per cloudy texel [measured] x the rows).
                  for (uint32_t row = 0; row < h; row += kBandRows)
                  {
                      const uint32_t rows = std::min(kBandRows, h - row);
                      uint32_t k[16] = { recordSrv, c.uav(radiance), 0xFFFFFFFFu, 4, w, h, 0, row, c.srv(transmittanceLut), c.uav(stats), c.uav(distance), c.srv(msTable),
                                         history ? c.srv(previousRadiance) : 0xFFFFFFFFu, history ? c.srv(previousDistance) : 0xFFFFFFFFu, blockTexel, sunSteps };
                      if (!history)
                      {
                          c.computeConstants(k, 16);
                          c.cmd->Dispatch(groups(w, 8), groups(rows, 8), 1);
                          continue;
                      }
                      // (CloudMarch.hlsl P[3].z: the frame's texel of every block in a dispatch of its own, then the others)
                      k[14] = blockTexel | 1u << 8;
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch(groups((w + 1) / 2, 8), groups((rows + 1) / 2, 8), 1);
                      k[14] = blockTexel | 2u << 8;
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch(groups(w, 8), groups(rows, 8), 1);
                  }
              });
    // The sky dome for R's escaping rays (GiSky.hlsli: atmosphereSkyRadianceCloudy): mode 3, a dispatch per run of bands
    // that are marched this frame (the whole dome: one dispatch, 24,576 texels; no band due: no pass).
    if (domeBands != 0)
        g.addPass("s.cloud.sky", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(record, Use::SrvCompute);
                      b.use(map, Use::SrvCompute);
                      b.use(transmittanceLut, Use::SrvCompute);
                      b.use(msTable, Use::SrvCompute);
                      b.use(dome, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(pc);
                      c.bindFrameConstants(cb);
                      for (uint32_t band = 0; band < kDomeBands;)
                      {
                          if ((domeBands >> band & 1u) == 0)
                          {
                              ++band;
                              continue;
                          }
                          uint32_t run = 1;
                          while (band + run < kDomeBands && (domeBands >> (band + run) & 1u) != 0) ++run;
                          const uint32_t k[16] = { recordSrv, c.uav(dome), 0xFFFFFFFFu, 1, kDomeWidth, kDomeHeight, 3, band * kDomeBandRows, c.srv(transmittanceLut),
                                                   0xFFFFFFFFu, 0xFFFFFFFFu, c.srv(msTable), 0xFFFFFFFFu, 0xFFFFFFFFu, 0, sunSteps };
                          c.computeConstants(k, 16);
                          c.cmd->Dispatch(groups(kDomeWidth, 8), run, 1);  // (a band is one row of 8 x 8 groups: kDomeBandRows)
                          band += run;
                      }
                  });
    // The readers (S's atmosphere functions in M's passes, R's sky) sample the layer through the record's SRVs: this pass
    // puts the textures in the shader-resource layout for them.
    g.addPass("s.cloud.publish", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(radiance, Use::SrvCompute);
                  b.use(distance, Use::SrvCompute);
                  b.use(dome, Use::SrvCompute);
                  b.use(map, Use::SrvCompute);
                  b.use(record, Use::SrvCompute);
                  b.keep();
              },
              [](PassContext&) {});
}

CloudStats cloudStats(TrackState& state)
{
    CloudState& s = state.get<CloudState>(kCloudKey);
    CloudStats st;
    st.frames = s.frames;
    if (!s.device || !s.stats) return st;
    Device& device = *s.device;
    device.waitIdle();
    if (!s.statsReadback) s.statsReadback = createBuffer(device, L"S cloud stats readback", 256, D3D12_HEAP_TYPE_READBACK);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(s.statsReadback.Get(), 0, s.stats.Get(), 0, 16);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    void* m = nullptr;
    D3D12_RANGE all{ 0, 16 }, none{ 0, 0 };
    check(s.statsReadback->Map(0, &all, &m), "map cloud stats");
    const uint32_t* v = static_cast<const uint32_t*>(m);
    st.cappedPixels = v[0], st.cappedSunPaths = v[1];
    s.statsReadback->Unmap(0, &none);
    return st;
}
} // namespace unx::render::atmosphere
