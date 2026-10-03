// UnravelNext.dll renderer C ABI (UnravelNextHost.h): converts the fixed-size ABI structs into scene:: types and
// forwards to HostRenderer. Every entry point catches exceptions and reports them through UnxLastError.
#include "unx/host/UnravelNextHost.h"

#include "Renderer/HostRenderer.h"
#include "GpuBridge/GpuBridge.h"
#include "Unity/PluginState.h"

#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D12.h"
#include "IUnityInterface.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

using namespace unx;
using namespace unx::host;
using unx::render::check;
using unx::render::ComPtr;

namespace
{
std::mutex g_renderersMutex;
std::map<uint64_t, std::shared_ptr<HostRenderer>> g_renderers;
uint64_t g_nextRenderer = 1;

std::shared_ptr<HostRenderer> find(UnxRenderer r)
{
    std::lock_guard lock(g_renderersMutex);
    auto it = g_renderers.find(r);
    if (it == g_renderers.end()) fail("unknown renderer handle %llu", (unsigned long long)r);
    return it->second;
}

// ABI tickets name their renderer, so UNX_EVENT_RENDER needs only the ticket: renderer handle << 40 | frame ticket.
constexpr uint32_t kTicketBits = 40;
uint64_t globalTicket(UnxRenderer r, uint64_t local)
{
    if (local >> kTicketBits) fail("frame ticket space of renderer %llu exhausted", (unsigned long long)r);
    return (r << kTicketBits) | local;
}
UnxRenderer ticketRenderer(uint64_t ticket) { return ticket >> kTicketBits; }
uint64_t ticketLocal(uint64_t ticket) { return ticket & ((1ull << kTicketBits) - 1); }

template <typename T>
void requireStruct(const T* p, const char* name)
{
    if (!p) fail("%s is null", name);
    if (p->size != sizeof(T) || p->version != 1) fail("%s ABI mismatch: size %u version %u, native %zu version 1", name, p->size, p->version, sizeof(T));
}

std::string fixedString(const char* s, size_t capacity)
{
    size_t n = 0;
    while (n < capacity && s[n]) ++n;
    if (n == capacity) fail("string field is not terminated within %zu bytes", capacity);
    return std::string(s, n);
}

std::filesystem::path utf8Path(const char* s, size_t capacity)
{
    const std::string text = fixedString(s, capacity);
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

float3 f3(const float* v) { return { v[0], v[1], v[2] }; }

float3x4 affine(const float* m)
{
    float3x4 a;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) a.m[r][c] = m[r * 4 + c];
    return a;
}

uint32_t texelBytes(scene::TextureFormat f)
{
    switch (f)
    {
    case scene::TextureFormat::Rgba8Srgb:
    case scene::TextureFormat::Rgba8Linear: return 4;
    case scene::TextureFormat::Rg8Normal:
    case scene::TextureFormat::Rg8RoughMetal: return 2;
    case scene::TextureFormat::R8Linear: return 1;
    case scene::TextureFormat::Rgba16Float: return 8;
    }
    return 0;
}

// Device removal (INTERFACES v1.27): the plugin runs with DeviceRemovedPolicy::Throw, so a removed device surfaces as
// DeviceRemovedError instead of ending the host process. From then on every renderer call answers UNX_DEVICE_REMOVED
// (the device cannot be used again); destroying a renderer still works, its waits return on a removed device.
template <typename F>
int32_t call(F&& f, bool afterRemoval = false)
{
    if (!afterRemoval && render::deviceWasRemoved())
    {
        plugin::failWith("the D3D12 device was removed; destroy the renderer (UnxRendererDestroy)");
        return UNX_DEVICE_REMOVED;
    }
    try
    {
        f();
        return UNX_OK;
    }
    catch (const render::DeviceRemovedError& e)
    {
        plugin::failWith(e.what());
        return UNX_DEVICE_REMOVED;
    }
    catch (const std::exception& e)
    {
        return plugin::failWith(e.what());
    }
}
} // namespace

UNX_API int32_t UNX_CALL UnxRendererCreate(const UnxRendererDesc* desc, UnxRenderer* renderer)
{
    return call([&] {
        requireStruct(desc, "UnxRendererDesc");
        if (!renderer) fail("renderer output is null");
        HostRendererOptions o;
        o.standalone = (desc->flags & UNX_RENDERER_STANDALONE) != 0;
        if (!o.standalone)
        {
            // Unity's device and graphics queue (host boundary decision). Load-time submissions (scene upload) go to the
            // same queue from the main thread; queue methods are thread-safe and those lists touch only our resources.
            IUnityGraphicsD3D12v8* d3d = plugin::unityD3D12();
            if (!d3d || !d3d->GetDevice() || !d3d->GetCommandQueue())
                fail("Unity's D3D12 device or queue is not available (plugin not loaded by Unity, or not D3D12)");
            o.hostDevice = d3d->GetDevice();
            o.hostQueue = d3d->GetCommandQueue();
        }
        // The one World -> renderer axis mapping of the particle stream (engine 2's N2 finding, 2026-09-27): Unity's World is
        // the renderer's world mirrored in z (the bridge's Space.ToAffine for everything else it sends).
        o.streamAxes[0] = 1, o.streamAxes[1] = 1, o.streamAxes[2] = -1;
        o.framesInFlight = desc->framesInFlight;
        o.shaderDirectory = utf8Path(desc->shaderDirectory, sizeof desc->shaderDirectory);
        o.qualityDirectory = utf8Path(desc->qualityDirectory, sizeof desc->qualityDirectory);
        auto r = std::make_shared<HostRenderer>(o);
        std::lock_guard lock(g_renderersMutex);
        *renderer = g_nextRenderer++;
        g_renderers[*renderer] = std::move(r);
    });
}

UNX_API int32_t UNX_CALL UnxRendererDestroy(UnxRenderer renderer)
{
    return call([&] {
        std::shared_ptr<HostRenderer> r;
        {
            std::lock_guard lock(g_renderersMutex);
            auto it = g_renderers.find(renderer);
            if (it == g_renderers.end()) fail("unknown renderer handle %llu", (unsigned long long)renderer);
            r = std::move(it->second);
            g_renderers.erase(it);
        }
        r.reset();  // waits for the GPU (HostRenderer destructor); returns at once on a removed device
    }, true);
}

UNX_API int32_t UNX_CALL UnxSceneAddTexture(UnxRenderer r, const UnxTextureDesc* d, uint32_t* index)
{
    return call([&] {
        requireStruct(d, "UnxTextureDesc");
        if (d->format > UNX_TEXTURE_RGBA16_FLOAT) fail("unknown texture format %u", d->format);
        scene::Texture t;
        t.name = fixedString(d->name, sizeof d->name);
        t.width = d->width;
        t.height = d->height;
        t.format = (scene::TextureFormat)d->format;
        t.wrap = d->wrap != 0;
        const uint64_t bytes = (uint64_t)d->width * d->height * texelBytes(t.format);
        if (!d->texels || d->byteCount != bytes || bytes == 0) fail("texture '%s': %llu bytes given, %ux%u needs %llu", t.name.c_str(), (unsigned long long)d->byteCount, d->width, d->height, (unsigned long long)bytes);
        t.texels.assign((const uint8_t*)d->texels, (const uint8_t*)d->texels + bytes);
        auto h = find(r);
        const uint32_t i = h->add(h->scene().textures, std::move(t));
        if (index) *index = i;
    });
}

static scene::Material toMaterial(const UnxMaterialDesc* d)
{
    // version 5 (sizeof), 4 (up to anisotropyRotation), 3 or 2 (up to attenuationDistance; 2 without the sheen and attenuation fields) or version 1
    // (without the layer fields)
    if (!d) fail("UnxMaterialDesc is null");
    const uint32_t v3Size = (uint32_t)offsetof(UnxMaterialDesc, anisotropy);
    const uint32_t v4Size = (uint32_t)offsetof(UnxMaterialDesc, thinFilmThickness);
    const uint32_t v5Size = (uint32_t)offsetof(UnxMaterialDesc, waterScattering);
    const uint32_t v6Size = (uint32_t)offsetof(UnxMaterialDesc, emissiveVisibleOnly);
    const bool v7 = d->size == sizeof(UnxMaterialDesc) && d->version == 7;
    const bool v6 = v7 || (d->size == v6Size && d->version == 6);
    const bool v5 = v6 || (d->size == v5Size && d->version == 5);
    const bool v4 = v5 || (d->size == v4Size && d->version == 4);
    const bool v3 = v4 || (d->size == v3Size && d->version == 3);
    const bool v2 = v3 || (d->size == v3Size && d->version == 2);
    if (!v2 && !(d->size == v3Size - 32 && d->version == 1))
        fail("UnxMaterialDesc ABI mismatch: size %u version %u, native %zu version 7 (or %u version 6, %u version 5, %u version 4, %u version 3 or 2, %u version 1)",
             d->size, d->version, sizeof(UnxMaterialDesc), v6Size, v5Size, v4Size, v3Size, v3Size - 32);
    if (d->materialClass > UNX_MATERIAL_TERRAIN) fail("unknown material class %u", d->materialClass);
    scene::Material m;
    m.name = fixedString(d->name, sizeof d->name);
    m.cls = (scene::MaterialClass)d->materialClass;
    m.baseColor = f3(d->baseColor);
    m.roughness = d->roughness;
    m.metallic = d->metallic;
    m.specular = d->specular;
    m.emissive = f3(d->emissive);
    m.alphaCutoff = d->alphaCutoff;
    m.transmission = d->transmission;
    m.ior = d->ior;
    m.twoSided = d->twoSided != 0;
    m.baseColorTexture = d->baseColorTexture;
    m.normalTexture = d->normalTexture;
    m.roughMetalTexture = d->roughMetalTexture;
    m.emissiveTexture = d->emissiveTexture;
    m.occlusionTexture = d->occlusionTexture;
    if (v2)
    {
        m.clearcoat = d->clearcoat;
        m.clearcoatRoughness = d->clearcoatRoughness;
        m.clearcoatIor = d->clearcoatIor;
    }
    if (v3)
    {
        m.sheenColor = f3(d->sheenColor);
        m.sheenRoughness = d->sheenRoughness;
        if (d->attenuationDistance > 0) m.attenuationDistance = d->attenuationDistance;
    }
    if (v4)
    {
        m.anisotropy = d->anisotropy;
        m.anisotropyRotation = d->anisotropyRotation;
    }
    if (v5)
    {
        m.thinFilmThickness = d->thinFilmThickness;
        m.thinFilmIor = d->thinFilmIor;
        m.thinFilmCoverage = d->thinFilmCoverage;
        m.thinFilmSubstrate = d->thinFilmSubstrate;
        m.substrateIor = d->substrateIor;
        m.substrateExtinction = d->substrateExtinction;
    }
    if (v6)
    {
        m.waterScattering = f3(d->waterScattering);
        m.waterAnisotropy = d->waterAnisotropy;
    }
    if (v7) m.emissiveVisibleOnly = d->emissiveVisibleOnly != 0;
    return m;
}

UNX_API int32_t UNX_CALL UnxSceneAddMaterial(UnxRenderer r, const UnxMaterialDesc* d, uint32_t* index)
{
    return call([&] {
        scene::Material m = toMaterial(d);
        auto h = find(r);
        const uint32_t i = h->add(h->scene().materials, std::move(m));
        if (index) *index = i;
    });
}

// The scene mesh of a mesh description (checks shared by scene meshes and C2b runtime meshes).
static scene::Mesh meshFromDesc(const UnxMeshDesc* d)
{
    requireStruct(d, "UnxMeshDesc");
    scene::Mesh m;
    m.name = fixedString(d->name, sizeof d->name);
    const uint32_t n = d->vertexCount;
    if (!d->positions || !d->normals || !d->indices || !d->submeshes || n == 0 || d->indexCount == 0 || d->submeshCount == 0)
        fail("mesh '%s': positions, normals, indices and submeshes are required", m.name.c_str());
    m.positions.resize(n);
    m.normals.resize(n);
    for (uint32_t v = 0; v < n; ++v)
    {
        m.positions[v] = f3(d->positions + 3 * v);
        m.normals[v] = f3(d->normals + 3 * v);
    }
    if (d->tangents)
    {
        m.tangents.resize(n);
        for (uint32_t v = 0; v < n; ++v) m.tangents[v] = { d->tangents[4 * v], d->tangents[4 * v + 1], d->tangents[4 * v + 2], d->tangents[4 * v + 3] };
    }
    if (d->uv0)
    {
        m.uv0.resize(n);
        for (uint32_t v = 0; v < n; ++v) m.uv0[v] = { d->uv0[2 * v], d->uv0[2 * v + 1] };
    }
    m.indices.assign(d->indices, d->indices + d->indexCount);
    for (uint32_t s = 0; s < d->submeshCount; ++s) m.submeshes.push_back({ d->submeshes[s].indexOffset, d->submeshes[s].indexCount, d->submeshes[s].material });
    if (d->jointCount)
    {
        if (!d->joints || !d->weights || !d->inverseBind) fail("mesh '%s': jointCount %u without joints, weights and inverseBind", m.name.c_str(), d->jointCount);
        m.skin.joints.assign(d->joints, d->joints + 4ull * n);
        m.skin.weights.assign(d->weights, d->weights + 4ull * n);
        for (uint32_t j = 0; j < d->jointCount; ++j) m.skin.inverseBind.push_back(affine(d->inverseBind + 12ull * j));
    }
    // Front faces are counter-clockwise: cross(b - a, c - a) points along the vertex normals. A mesh whose winding
    // opposes its own normals almost everywhere is an exporter convention error (e.g. a handedness mirror without
    // reversing the triangle order), and every renderer path would shade it from behind.
    uint64_t agree = 0, oppose = 0;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
    {
        const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        if (a >= n || b >= n || c >= n) fail("mesh '%s': index out of range (%u vertices)", m.name.c_str(), n);
        const float3 g = cross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a]);
        const float s = dot(g, m.normals[a] + m.normals[b] + m.normals[c]);
        if (s > 0) ++agree;
        else if (s < 0) ++oppose;
    }
    if (oppose > 0 && oppose >= 99 * agree)
        fail("mesh '%s': %llu of %llu triangles wind against their vertex normals; front faces are counter-clockwise in renderer space (a Z-mirrored "
             "exporter reverses each triangle: a, c, b)",
             m.name.c_str(), (unsigned long long)oppose, (unsigned long long)(agree + oppose));
    return m;
}

UNX_API int32_t UNX_CALL UnxSceneAddMesh(UnxRenderer r, const UnxMeshDesc* d, uint32_t* index)
{
    return call([&] {
        scene::Mesh m = meshFromDesc(d);
        auto h = find(r);
        const uint32_t i = h->add(h->scene().meshes, std::move(m));
        if (index) *index = i;
    });
}

UNX_API int32_t UNX_CALL UnxSceneAddSkeleton(UnxRenderer r, const float* jointToModel, uint32_t jointCount, uint32_t* index)
{
    return call([&] {
        if (!jointToModel || jointCount == 0) fail("skeleton needs at least one joint");
        scene::Skeleton s;
        for (uint32_t j = 0; j < jointCount; ++j) s.jointToModel.push_back(affine(jointToModel + 12ull * j));
        auto h = find(r);
        s.name = "skeleton" + std::to_string(h->scene().skeletons.size());
        const uint32_t i = h->add(h->scene().skeletons, std::move(s));
        if (index) *index = i;
    });
}

static scene::Instance toInstance(const UnxInstanceDesc* d)
{
    requireStruct(d, "UnxInstanceDesc");
    scene::Instance inst;
    inst.mesh = d->mesh;
    inst.transform = affine(d->transform);
    inst.flags = d->flags;
    inst.skeleton = d->skeleton;
    inst.wind = { d->windStiffness, d->windPhase, d->windAnchorHeight };
    if (d->materialOverrideCount)
    {
        if (!d->materialOverrides) fail("instance: materialOverrideCount %u without materialOverrides", d->materialOverrideCount);
        inst.materialOverrides.assign(d->materialOverrides, d->materialOverrides + d->materialOverrideCount);
    }
    return inst;
}

namespace
{
// V3: NV_StreamExecutor callbacks on a renderer (user = its HostRenderer; the bridge detaches before destroying it).
// Result codes of NativeVfx.h (NV_OK, NV_ARGUMENT, NV_INTERNAL; the stream header does not carry them).
constexpr int32_t NV_OK = 0, NV_ARGUMENT = 1, NV_INTERNAL = 10;
int32_t vfxSubmitCallback(void* user, const uint8_t* packet, uint64_t bytes)
{
    try { static_cast<HostRenderer*>(user)->vfxSubmit(packet, bytes); }
    catch (const std::exception& e) { logf("UnravelNext FX executor submit: %s\n", e.what()); }
    return NV_OK;  // a failure is reported by the next readback (the commit must not fail)
}
int32_t vfxReadbackCallback(void* user, uint64_t stream, uint64_t generation, uint64_t tick, NV_StreamReadback* output)
{
    try
    {
        if (!output || output->size < sizeof(NV_StreamReadback)) return NV_ARGUMENT;
        const fx::TickReadback& r = static_cast<HostRenderer*>(user)->vfxReadback(stream, generation, tick);
        output->counters = r.counters;
        output->events = r.events.data();
        output->event_count = r.events.size();
        return NV_OK;
    }
    catch (const std::exception& e)
    {
        logf("UnravelNext FX executor readback (tick %llu): %s\n", (unsigned long long)tick, e.what());
        return NV_INTERNAL;
    }
}
int32_t vfxCheckpointOrientationsCallback(void* user, uint64_t stream, uint64_t generation, uint64_t tick, const NV_StreamParticleOrientation** records, uint64_t* count)
{
    try
    {
        if (!records || !count) return NV_ARGUMENT;
        const std::vector<NV_StreamParticleOrientation>& r = static_cast<HostRenderer*>(user)->vfxCheckpointOrientations(stream, generation, tick);
        *records = r.data();
        *count = r.size();
        return NV_OK;
    }
    catch (const std::exception& e)
    {
        logf("UnravelNext FX executor checkpoint orientations (tick %llu): %s\n", (unsigned long long)tick, e.what());
        return NV_INTERNAL;
    }
}

int32_t vfxCheckpointCallback(void* user, uint64_t stream, uint64_t generation, uint64_t tick, const NV_StreamParticle** records, uint64_t* count)
{
    try
    {
        if (!records || !count) return NV_ARGUMENT;
        const std::vector<NV_StreamParticle>& r = static_cast<HostRenderer*>(user)->vfxCheckpoint(stream, generation, tick);
        *records = r.data();
        *count = r.size();
        return NV_OK;
    }
    catch (const std::exception& e)
    {
        logf("UnravelNext FX executor checkpoint (tick %llu): %s\n", (unsigned long long)tick, e.what());
        return NV_INTERNAL;
    }
}
void vfxDetachCallback(void*, uint64_t) {}
} // namespace

UNX_API int32_t UNX_CALL UnxRendererQualityOverride(UnxRenderer r, const char* utf8Assignment)
{
    return call([&] {
        if (!utf8Assignment) fail("UnxRendererQualityOverride: no assignment");
        find(r)->overrideQuality(utf8Assignment);
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetLens(UnxRenderer r, float apertureMetres, float focusMetres)
{
    return call([&] { find(r)->setLens(apertureMetres, focusMetres); });
}

UNX_API int32_t UNX_CALL UnxFrameSetWhiteBalance(UnxRenderer r, float kelvin, float tint)
{
    return call([&] { find(r)->setWhiteBalance(kelvin, tint); });
}

// A3 mesh particles (render C, v1.82): the scene mesh a stream program's mesh_asset draws - a committed mesh index, a
// runtime mesh id (UnxSceneAddRuntimeMesh, bit 31 set) or 0xFFFFFFFF to remove the mapping. Takes effect from the next
// queued frame; particles of an unmapped asset (or of a removed runtime mesh) are not drawn and counted unmapped.
UNX_API int32_t UNX_CALL UnxVfxMapMeshAsset(UnxRenderer r, uint64_t asset, uint32_t mesh)
{
    return call([&] { find(r)->mapMeshAsset(asset, mesh); });
}

UNX_API int32_t UNX_CALL UnxSurfaceDelta(UnxRenderer r, const void* changed, uint64_t changedCount, const int32_t* removedKeys, uint64_t removedCount)
{
    static_assert(sizeof(surface::BrickInput) == 1560, "NV_SurfaceBrickV2");
    return call([&] { find(r)->surfaceDelta(static_cast<const surface::BrickInput*>(changed), (size_t)changedCount, removedKeys, (size_t)removedCount); });
}

UNX_API int32_t UNX_CALL UnxSurfaceSetHalfLives(UnxRenderer r, const double* halfLives6)
{
    return call([&] {
        if (!halfLives6) fail("UnxSurfaceSetHalfLives: null");
        std::array<double, surface::kChannels> h;
        std::copy(halfLives6, halfLives6 + surface::kChannels, h.begin());
        find(r)->setSurfaceHalfLives(h);
    });
}

UNX_API int32_t UNX_CALL UnxSurfaceSetTime(UnxRenderer r, double seconds)
{
    return call([&] { find(r)->setSurfaceTime(seconds); });
}

UNX_API int32_t UNX_CALL UnxDebugPrimitives(UnxRenderer r, const void* lines, uint32_t lineCount, const void* triangles, uint32_t triangleCount)
{
    return call([&] {
        if ((lineCount && !lines) || (triangleCount && !triangles)) fail("UnxDebugPrimitives: null records");
        find(r)->debugPrimitives({ static_cast<const debug::Line*>(lines), lineCount }, { static_cast<const debug::Triangle*>(triangles), triangleCount }, {});
    });
}

UNX_API int32_t UNX_CALL UnxDebugText(UnxRenderer r, const float* anchor3, const char* utf8, uint32_t rgba, float sizePx, uint32_t flags, float offsetX, float offsetY)
{
    return call([&] {
        if (!anchor3 || !utf8) fail("UnxDebugText: null anchor or text");
        find(r)->debugText(float3{ anchor3[0], anchor3[1], anchor3[2] }, utf8, rgba, sizePx, flags, float2{ offsetX, offsetY });
    });
}

namespace
{
decal::Decal decalOf(const UnxDecalDesc* desc)
{
    if (!desc) fail("decal: null description");
    if (desc->reserved != 0) fail("decal: reserved is not 0");
    decal::Decal d;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) d.box.m[r][c] = desc->box[r * 4 + c];
    d.material = desc->material;
    d.instance = desc->instance;
    d.priority = desc->priority;
    d.opacity = desc->opacity;
    d.fadeStartDegrees = desc->fadeStartDegrees;
    d.fadeEndDegrees = desc->fadeEndDegrees;
    d.edge = desc->edge;
    return d;
}
} // namespace

UNX_API int32_t UNX_CALL UnxDecalAdd(UnxRenderer r, const UnxDecalDesc* desc, uint32_t* id)
{
    return call([&] {
        if (!id) fail("UnxDecalAdd: null id");
        *id = find(r)->decalAdd(decalOf(desc));
    });
}

UNX_API int32_t UNX_CALL UnxDecalUpdate(UnxRenderer r, uint32_t id, const UnxDecalDesc* desc)
{
    return call([&] { find(r)->decalUpdate(id, decalOf(desc)); });
}

UNX_API int32_t UNX_CALL UnxDecalRemove(UnxRenderer r, uint32_t id)
{
    return call([&] { find(r)->decalRemove(id); });
}

namespace
{
float3x4 poseOf(const float* m12)
{
    if (!m12) fail("view model: null pose");
    float3x4 p;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) p.m[r][c] = m12[r * 4 + c];
    return p;
}
} // namespace

UNX_API int32_t UNX_CALL UnxViewModelAdd(UnxRenderer r, uint32_t instance, const float* cameraLocal12, uint32_t* id)
{
    return call([&] {
        if (!id) fail("UnxViewModelAdd: null id");
        *id = find(r)->viewModelAdd(instance, poseOf(cameraLocal12));
    });
}

UNX_API int32_t UNX_CALL UnxViewModelSetPose(UnxRenderer r, uint32_t id, const float* cameraLocal12)
{
    return call([&] { find(r)->viewModelSetPose(id, poseOf(cameraLocal12)); });
}

UNX_API int32_t UNX_CALL UnxViewModelRemove(UnxRenderer r, uint32_t id)
{
    return call([&] { find(r)->viewModelRemove(id); });
}

namespace
{
std::filesystem::path utf8Path(const char* s)
{
    const std::string p(s);
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(p.data()), p.size()));
}
} // namespace

UNX_API int32_t UNX_CALL UnxFrameSetFluids(UnxRenderer r, const UnxFluidInput* fluids, uint32_t count, const uint64_t stamp[6])
{
    return call([&] {
        if ((count && !fluids) || !stamp) fail("UnxFrameSetFluids: missing arrays");
        std::vector<HostRenderer::FluidInput> in(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            in[i].view = fluids[i].view;
            in[i].alpha = fluids[i].alpha;
            std::memcpy(in[i].domainCells, fluids[i].domainCells, sizeof in[i].domainCells);
            in[i].material = fluids[i].material;
        }
        uint64_t s[6];
        std::memcpy(s, stamp, sizeof s);
        find(r)->setFluids(in, s);
    });
}

UNX_API int32_t UNX_CALL UnxSceneSetTerrainLayers(UnxRenderer r, uint32_t material, uint32_t splat0, uint32_t splat1, const UnxTerrainLayer* layers, uint32_t count)
{
    return call([&] {
        if (count && !layers) fail("UnxSceneSetTerrainLayers: no layers");
        auto h = find(r);
        std::vector<scene::TerrainLayer> in(count);
        for (uint32_t i = 0; i < count; ++i)
            in[i] = { layers[i].material, float2{ layers[i].scale[0], layers[i].scale[1] }, float2{ layers[i].offset[0], layers[i].offset[1] } };
        h->setTerrainLayers(material, splat0, splat1, in);
    });
}

UNX_API int32_t UNX_CALL UnxSceneSetCharacterShading(UnxRenderer r, uint32_t material, const UnxCharacterShadingDesc* d)
{
    return call([&] {
        if (!d) fail("UnxSceneSetCharacterShading: no description");
        if (d->size != sizeof(UnxCharacterShadingDesc) || d->version != 1)
            fail("UnxSceneSetCharacterShading: UnxCharacterShadingDesc size %u version %u", d->size, d->version);
        host::CharacterShading c;  // (the engine's defaults; a 0 marked "default" in the header keeps them)
        if (d->subsurfaceMeanFreePath[0] != 0 || d->subsurfaceMeanFreePath[1] != 0 || d->subsurfaceMeanFreePath[2] != 0) c.subsurfaceMeanFreePath = f3(d->subsurfaceMeanFreePath);
        if (d->subsurfaceLobeRoughness[0] != 0 || d->subsurfaceLobeRoughness[1] != 0)
        {
            c.subsurfaceLobeMix = d->subsurfaceLobeMix;
            c.subsurfaceLobeRoughness = { d->subsurfaceLobeRoughness[0], d->subsurfaceLobeRoughness[1] };
        }
        c.cloth = d->cloth;
        c.eyeIrisRadius = d->eyeIrisRadius;
        if (d->eyeIrisDepth != 0) c.eyeIrisDepth = d->eyeIrisDepth;
        if (d->eyeLimbusWidth != 0) c.eyeLimbusWidth = d->eyeLimbusWidth;
        c.eyeLimbusDarkening = d->eyeLimbusDarkening;
        if (d->eyePupilScale != 0) c.eyePupilScale = d->eyePupilScale;
        c.eyeIrisConcavity = d->eyeIrisConcavity;
        if (d->eyeIor != 0) c.eyeIor = d->eyeIor;
        if (d->eyeAxis[0] != 0 || d->eyeAxis[1] != 0 || d->eyeAxis[2] != 0) c.eyeAxis = f3(d->eyeAxis);
        find(r)->setCharacterShading(material, c);
    });
}

UNX_API int32_t UNX_CALL UnxMaterialInputsDefaults(UnxMaterialInputsDesc* d)
{
    return call([&] {
        if (!d) fail("UnxMaterialInputsDesc output is null");
        const host::MaterialInputs in;
        std::memset(d, 0, sizeof *d);
        d->size = sizeof *d;
        d->version = 1;
        d->uvScale[0] = in.uvScale.x, d->uvScale[1] = in.uvScale.y;
        d->detailScale[0] = in.detailScale.x, d->detailScale[1] = in.detailScale.y;
        d->detailColorTexture = d->detailNormalTexture = d->heightTexture = d->emissiveMaskTexture = UNX_NONE;
        d->detailColorStrength = in.detailColorStrength;
        d->detailNormalScale = in.detailNormalScale;
        d->emissiveScale = in.emissiveScale;
    });
}

UNX_API int32_t UNX_CALL UnxSceneSetMaterialInputs(UnxRenderer r, uint32_t material, const UnxMaterialInputsDesc* d)
{
    return call([&] {
        if (!d) fail("UnxSceneSetMaterialInputs: no description");
        if (d->size != sizeof(UnxMaterialInputsDesc) || d->version != 1)
            fail("UnxSceneSetMaterialInputs: UnxMaterialInputsDesc size %u version %u", d->size, d->version);
        if ((d->flags & ~(UNX_MATERIAL_INPUT_VERTEX_TINT | UNX_MATERIAL_INPUT_VERTEX_BLEND | UNX_MATERIAL_INPUT_ALPHA_DITHER)) != 0)
            fail("UnxSceneSetMaterialInputs: unknown flags 0x%x", d->flags);
        host::MaterialInputs in;
        in.uvScale = { d->uvScale[0], d->uvScale[1] };
        in.uvOffset = { d->uvOffset[0], d->uvOffset[1] };
        in.uvRotation = d->uvRotation;
        in.occlusionUvSet = d->occlusionUvSet;
        in.detailColorTexture = d->detailColorTexture;
        in.detailNormalTexture = d->detailNormalTexture;
        in.detailScale = { d->detailScale[0], d->detailScale[1] };
        in.detailOffset = { d->detailOffset[0], d->detailOffset[1] };
        in.detailUvSet = d->detailUvSet;
        in.detailColorStrength = d->detailColorStrength;
        in.detailNormalScale = d->detailNormalScale;
        in.heightTexture = d->heightTexture;
        in.heightScale = d->heightScale;
        in.emissiveScale = d->emissiveScale;
        in.emissiveMaskTexture = d->emissiveMaskTexture;
        in.vertexColorTint = (d->flags & UNX_MATERIAL_INPUT_VERTEX_TINT) != 0;
        in.vertexAlphaBlend = (d->flags & UNX_MATERIAL_INPUT_VERTEX_BLEND) != 0;
        in.alphaDither = (d->flags & UNX_MATERIAL_INPUT_ALPHA_DITHER) != 0;
        find(r)->setMaterialInputs(material, in);
    });
}

UNX_API int32_t UNX_CALL UnxSceneSetMeshAttributes(UnxRenderer r, uint32_t mesh, const float* uv1, const uint32_t* colors, uint32_t vertexCount)
{
    return call([&] {
        if (!uv1 && !colors) fail("UnxSceneSetMeshAttributes: neither a second uv set nor colours");
        std::vector<float2> uvs;
        if (uv1)
        {
            uvs.resize(vertexCount);
            for (uint32_t v = 0; v < vertexCount; ++v) uvs[v] = { uv1[2 * v], uv1[2 * v + 1] };
        }
        std::vector<uint32_t> cols;
        if (colors) cols.assign(colors, colors + vertexCount);
        find(r)->setMeshAttributes(mesh, std::move(uvs), std::move(cols));
    });
}

UNX_API int32_t UNX_CALL UnxLightComponentsDefaults(UnxLightComponentsDesc* d)
{
    return call([&] {
        if (!d) fail("UnxLightComponentsDesc output is null");
        const scene::Light l;
        std::memset(d, 0, sizeof *d);
        d->size = sizeof *d;
        d->version = 1;
        d->specularScale = l.specularScale;
        d->diffuseScale = l.diffuseScale;
        d->volumetricScattering = l.volumetricScattering;
        d->indirectIntensity = l.indirectIntensity;
        d->sourceTexture = UNX_NONE;
        d->barnDoorAngle = l.barnDoorAngle;
        d->barnDoorLength = l.barnDoorLength;
        d->lightingChannels = l.lightingChannels;
        d->maxDrawDistance = l.maxDrawDistance;
        d->maxDistanceFadeRange = l.maxDistanceFadeRange;
        d->temperature = l.temperature;
        d->falloffExponent = l.falloffExponent;
    });
}

UNX_API int32_t UNX_CALL UnxSceneSetLightComponents(UnxRenderer r, uint32_t light, const UnxLightComponentsDesc* d)
{
    return call([&] {
        if (!d) fail("UnxSceneSetLightComponents: no description");
        if (d->size != sizeof(UnxLightComponentsDesc) || d->version != 1)
            fail("UnxSceneSetLightComponents: UnxLightComponentsDesc size %u version %u", d->size, d->version);
        scene::Light c;
        c.specularScale = d->specularScale;
        c.diffuseScale = d->diffuseScale;
        c.volumetricScattering = d->volumetricScattering;
        c.indirectIntensity = d->indirectIntensity;
        c.sourceTexture = d->sourceTexture;
        c.barnDoorAngle = d->barnDoorAngle;
        c.barnDoorLength = d->barnDoorLength;
        c.lightingChannels = d->lightingChannels;
        c.maxDrawDistance = d->maxDrawDistance;
        c.maxDistanceFadeRange = d->maxDistanceFadeRange;
        c.temperature = d->temperature;
        c.falloffExponent = d->falloffExponent;
        find(r)->setLightComponents(light, c);
    });
}

UNX_API int32_t UNX_CALL UnxSceneSetInstanceLightingChannels(UnxRenderer r, uint32_t instance, uint32_t channels)
{
    return call([&] { find(r)->setInstanceLightingChannels(instance, channels); });
}

UNX_API int32_t UNX_CALL UnxFrameSetClouds(UnxRenderer r, const UnxCloudDesc* clouds)
{
    return call([&] {
        render::CloudLayerDesc c;  // null: no clouds (coverage 0)
        if (clouds)
        {
            if (clouds->size != sizeof(UnxCloudDesc) || clouds->version != 1) fail("UnxFrameSetClouds: UnxCloudDesc size %u version %u", clouds->size, clouds->version);
            c.coverage = clouds->coverage;
            c.baseAltitude = clouds->baseAltitude, c.topAltitude = clouds->topAltitude;
            c.sigmaMax = clouds->sigmaMax, c.albedo = clouds->albedo;
            c.windX = clouds->windX, c.windZ = clouds->windZ;
            c.seed = clouds->seed;
        }
        find(r)->setClouds(c);
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetFog(UnxRenderer r, const UnxFogDesc* fog)
{
    return call([&] {
        render::FogDesc f;  // null: the quality file's fog
        if (fog)
        {
            if (fog->size != sizeof(UnxFogDesc) || fog->version != 1) fail("UnxFrameSetFog: UnxFogDesc size %u version %u", fog->size, fog->version);
            f.enabled = true;
            f.density = fog->density, f.heightFalloff = fog->heightFalloff, f.height = fog->height;
            f.albedo[0] = fog->albedo[0], f.albedo[1] = fog->albedo[1], f.albedo[2] = fog->albedo[2];
            f.phaseG = fog->phaseG, f.startDistance = fog->startDistance, f.skyAmount = fog->skyAmount;
            f.noiseAmount = fog->noiseAmount, f.noiseScale = fog->noiseScale;
        }
        find(r)->setFog(f);
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetFogVolumes(UnxRenderer r, const UnxFogVolumeDesc* volumes, uint32_t count)
{
    return call([&] {
        if (count && !volumes) fail("UnxFrameSetFogVolumes: no volumes");
        std::vector<render::FogVolumeDesc> in(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const UnxFogVolumeDesc& d = volumes[i];
            if (d.size != sizeof(UnxFogVolumeDesc) || d.version != 1) fail("UnxFrameSetFogVolumes: UnxFogVolumeDesc %u size %u version %u", i, d.size, d.version);
            render::FogVolumeDesc& v = in[i];
            for (int k = 0; k < 3; ++k) v.centre[k] = d.centre[k], v.halfSize[k] = d.halfSize[k], v.albedo[k] = d.albedo[k];
            v.yaw = d.yaw, v.shape = d.shape, v.density = d.density, v.heightFalloff = d.heightFalloff, v.edge = d.edge;
        }
        find(r)->setFogVolumes(in);
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetOcean(UnxRenderer r, const UnxOceanDesc* ocean)
{
    return call([&] {
        if (!ocean)
        {
            find(r)->setOcean(nullptr);
            return;
        }
        if (ocean->size != sizeof(UnxOceanDesc) || ocean->version != 1) fail("UnxFrameSetOcean: UnxOceanDesc size %u version %u", ocean->size, ocean->version);
        HostRenderer::OceanInput in;
        in.windSpeed = ocean->windSpeed, in.windDirection = ocean->windDirection, in.fetch = ocean->fetch, in.spread = ocean->spread;
        in.seed = ocean->seed;
        in.level = ocean->level;
        in.horizontalBound = ocean->horizontalBound, in.verticalBound = ocean->verticalBound;
        in.lake = ocean->lake != 0;
        in.lakeCentre[0] = ocean->lakeCentre[0], in.lakeCentre[1] = ocean->lakeCentre[1];
        in.lakeRadius = ocean->lakeRadius;
        find(r)->setOcean(&in);
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetPools(UnxRenderer r, const UnxPoolDesc* pools, uint32_t count)
{
    return call([&] {
        if (count && !pools) fail("UnxFrameSetPools: no basins");
        std::vector<HostRenderer::PoolInput> in(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const UnxPoolDesc& d = pools[i];
            if (d.size != sizeof(UnxPoolDesc) || d.version != 1) fail("UnxFrameSetPools: UnxPoolDesc %u size %u version %u", i, d.size, d.version);
            HostRenderer::PoolInput& p = in[i];
            p.id = d.id, p.material = d.material, p.shape = d.shape;
            p.sizeX = d.sizeX, p.sizeZ = d.sizeZ, p.depth = d.depth, p.surfaceFilm = d.surfaceFilm;
            p.centre[0] = d.centre[0], p.centre[1] = d.centre[1], p.centre[2] = d.centre[2];
            p.yaw = d.yaw;
        }
        find(r)->setPools(in);
    });
}

UNX_API int32_t UNX_CALL UnxFrameAddPoolSources(UnxRenderer r, const UnxPoolSource* sources, uint32_t count)
{
    return call([&] {
        if (count && !sources) fail("UnxFrameAddPoolSources: no sources");
        std::vector<FramePacket::PoolSource> in(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const UnxPoolSource& s = sources[i];
            in[i].pool = s.pool;
            in[i].source = render::PoolSourceFrame{ s.x, s.z, s.radius, s.impulse, s.volume };
        }
        find(r)->addPoolSources(in);
    });
}

UNX_API int32_t UNX_CALL UnxPoolStatsLatest(UnxRenderer r, UnxPoolStats* stats)
{
    return call([&] {
        if (!stats || stats->size != sizeof(UnxPoolStats) || stats->version != 1) fail("UnxPoolStatsLatest: UnxPoolStats size or version");
        water::PoolStats s;
        const bool valid = find(r)->poolStats(stats->pool, s);
        stats->valid = valid ? 1u : 0u;
        stats->frameIndex = valid ? s.frame : 0;
        stats->time = valid ? s.time : 0.0;
        stats->mean = valid ? s.mean : 0.0f;
        stats->rms = valid ? s.rms : 0.0f;
        stats->maxDeviation = valid ? s.maxDeviation : 0.0f;
        stats->reserved = 0;
    });
}

UNX_API int32_t UNX_CALL UnxHairAddBody(UnxRenderer r, const UnxHairBodyDesc* desc, uint32_t* body)
{
    return call([&] {
        if (!desc || desc->size != sizeof(UnxHairBodyDesc) || desc->version != 1 || !body) fail("UnxHairAddBody: UnxHairBodyDesc size or version, or no body");
        if (!desc->restPositions || !desc->guideJoint || (desc->follows && !desc->followStrands)) fail("UnxHairAddBody: missing arrays");
        hair::BodyDesc d;
        d.nodesPerStrand = desc->nodesPerStrand;
        d.joints = desc->joints;
        const size_t nodes = (size_t)desc->guides * desc->nodesPerStrand;
        for (size_t i = 0; i < nodes; ++i) d.restPositions.push_back({ desc->restPositions[3 * i], desc->restPositions[3 * i + 1], desc->restPositions[3 * i + 2] });
        d.guideJoint.assign(desc->guideJoint, desc->guideJoint + desc->guides);
        for (uint32_t f = 0; f < desc->follows; ++f)
        {
            const UnxHairFollow& x = desc->followStrands[f];
            d.follows.push_back({ x.guide, { x.offset[0], x.offset[1], x.offset[2] }, x.tipSpread });
        }
        d.rootRadius = desc->rootRadius;
        d.tipRadius = desc->tipRadius;
        d.material = desc->material;
        d.instance = desc->instance;
        const UnxHairSimulation& s = desc->simulation;
        d.params.gravity = { s.gravity[0], s.gravity[1], s.gravity[2] };
        d.params.damping = s.damping;
        d.params.globalStiffness = s.globalStiffness;
        d.params.globalRange = s.globalRange;
        d.params.localStiffness = s.localStiffness;
        d.params.localIterations = s.localIterations;
        d.params.dftlDamping = s.dftlDamping;
        d.params.collisionMargin = s.collisionMargin;
        d.params.substeps = s.substeps;
        d.params.windDrag = s.windDrag;
        *body = find(r)->hairAddBody(d);
    });
}

UNX_API int32_t UNX_CALL UnxHairTick(UnxRenderer r, uint32_t body, const float* joints12, uint32_t jointCount, const UnxHairCapsule* capsules,
                                     uint32_t capsuleCount, const float wind[3], float dt)
{
    return call([&] {
        if ((jointCount && !joints12) || (capsuleCount && !capsules) || !wind) fail("UnxHairTick: missing arrays");
        std::vector<float3x4> joints(jointCount);
        for (uint32_t j = 0; j < jointCount; ++j) joints[j] = poseOf(joints12 + 12 * j);
        std::vector<hair::Capsule> caps(capsuleCount);
        for (uint32_t k = 0; k < capsuleCount; ++k)
        {
            caps[k].a = { capsules[k].a[0], capsules[k].a[1], capsules[k].a[2] };
            caps[k].radius = capsules[k].radius;
            caps[k].b = { capsules[k].b[0], capsules[k].b[1], capsules[k].b[2] };
        }
        find(r)->hairTick(body, joints, caps, { wind[0], wind[1], wind[2] }, dt);
    });
}

UNX_API int32_t UNX_CALL UnxHairSetFrameFraction(UnxRenderer r, float fraction)
{
    return call([&] { find(r)->hairSetFrameFraction(fraction); });
}

UNX_API int32_t UNX_CALL UnxHairRemoveBody(UnxRenderer r, uint32_t body)
{
    return call([&] { find(r)->hairRemoveBody(body); });
}

UNX_API int32_t UNX_CALL UnxAcquireGpuBridge(UnxRenderer r, NRC_GpuBridge* bridge, uint32_t size)
{
    return call([&] {
        if (!bridge || size != sizeof(NRC_GpuBridge)) fail("UnxAcquireGpuBridge: NRC_GpuBridge of %u bytes, this build's is %zu", size, sizeof(NRC_GpuBridge));
        *bridge = find(r)->gpuBridge().acquire();
    });
}

UNX_API int32_t UNX_CALL UnxReleaseGpuBridge(NRC_GpuBridge* bridge, uint32_t size)
{
    return call([&] {
        if (!bridge || size != sizeof(NRC_GpuBridge)) fail("UnxReleaseGpuBridge: NRC_GpuBridge of %u bytes, this build's is %zu", size, sizeof(NRC_GpuBridge));
        if (bridge->release && bridge->context) bridge->release(bridge->context);
        std::memset(bridge, 0, sizeof *bridge);
    });
}

UNX_API int32_t UNX_CALL UnxGpuBridgeStatistics(UnxRenderer r, NRC_GpuStatistics* statistics, uint32_t size)
{
    return call([&] {
        if (!statistics || size != sizeof(NRC_GpuStatistics))
            fail("UnxGpuBridgeStatistics: NRC_GpuStatistics of %u bytes, this build's is %zu", size, sizeof(NRC_GpuStatistics));
        *statistics = find(r)->gpuBridge().statistics();
    });
}

UNX_API int32_t UNX_CALL UnxPhotoBegin(UnxRenderer r, const UnxPhotoDesc* desc)
{
    return call([&] {
        if (!desc || desc->size != sizeof(UnxPhotoDesc) || desc->version != 1) fail("UnxPhotoBegin: UnxPhotoDesc size %u version %u", desc ? desc->size : 0, desc ? desc->version : 0);
        scene::Camera c;
        c.name = "photo";
        c.position = f3(desc->camera.position);
        c.forward = f3(desc->camera.forward);
        c.up = f3(desc->camera.up);
        c.verticalFov = desc->camera.verticalFov;
        c.nearPlane = desc->camera.nearPlane;
        c.ev100 = desc->camera.ev100;
        PhotoSettings s;
        s.samplesPerPixel = desc->samplesPerPixel;
        s.halfSamplesPerFrame = desc->halfSamplesPerFrame;
        find(r)->photoBegin(c, s);
    });
}

UNX_API int32_t UNX_CALL UnxPhotoSave(UnxRenderer r, const char* utf8Exr, const char* utf8Png)
{
    return call([&] {
        find(r)->photoSave(utf8Exr && *utf8Exr ? utf8Path(utf8Exr) : std::filesystem::path(), utf8Png && *utf8Png ? utf8Path(utf8Png) : std::filesystem::path());
    });
}

UNX_API int32_t UNX_CALL UnxPhotoEnd(UnxRenderer r)
{
    return call([&] { find(r)->photoEnd(); });
}

UNX_API int32_t UNX_CALL UnxPhotoGetStatus(UnxRenderer r, UnxPhotoStatus* status)
{
    return call([&] {
        if (!status || status->size != sizeof(UnxPhotoStatus) || status->version != 1) fail("UnxPhotoGetStatus: UnxPhotoStatus size or version");
        const PhotoStatus s = find(r)->photoStatus();
        status->active = s.active ? 1u : 0u;
        status->width = s.width, status->height = s.height, status->samples = s.samples, status->target = s.target;
        status->saves = s.saves;
        status->relMse = s.relMse;
        status->startSeconds = s.startSeconds;
        status->generation = s.generation;
        const size_t n = std::min(s.error.size(), sizeof status->error - 1);
        std::memcpy(status->error, s.error.data(), n);
        status->error[n] = 0;
    });
}

UNX_API int32_t UNX_CALL UnxVfxStreamExecutor(UnxRenderer r, void* executor)
{
    return call([&] {
        if (!executor) fail("UnxVfxStreamExecutor: no executor");
        auto h = find(r);
        if (!h->committed()) fail("UnxVfxStreamExecutor: commit the scene first");
        NV_StreamExecutor e{};
        e.size = sizeof(NV_StreamExecutor);
        e.version = NV_STREAM_EXECUTOR_MESH_ORIENTATION;  // heightfield sections, World wind turbulence and mesh particle
                                                          // orientation (FX ParticleSystem, Particles.hlsli / MeshOrientation.hlsli)
        e.user = h.get();
        e.submit = vfxSubmitCallback;
        e.readback = vfxReadbackCallback;
        e.checkpoint = vfxCheckpointCallback;
        e.detach = vfxDetachCallback;
        e.checkpoint_orientations = vfxCheckpointOrientationsCallback;
        std::memcpy(executor, &e, sizeof e);  // 56 B (the version-4 struct; NativeVfx 87056534 and later)
    });
}

UNX_API int32_t UNX_CALL UnxSceneEditInstances(UnxRenderer r, const uint32_t* indices, const UnxInstanceDesc* descs, uint32_t count)
{
    return call([&] {
        if (count && (!indices || !descs)) fail("UnxSceneEditInstances: %u edits without indices or descriptions", count);
        std::vector<std::pair<uint32_t, scene::Instance>> edits;
        for (uint32_t k = 0; k < count; ++k) edits.emplace_back(indices[k], toInstance(&descs[k]));
        find(r)->editInstances(edits);
    });
}

UNX_API int32_t UNX_CALL UnxSceneEditMaterials(UnxRenderer r, const uint32_t* indices, const UnxMaterialDesc* descs, uint32_t count)
{
    return call([&] {
        if (count && (!indices || !descs)) fail("UnxSceneEditMaterials: %u edits without indices or descriptions", count);
        std::vector<std::pair<uint32_t, scene::Material>> edits;
        // the array's stride is its descriptions' size (version 1 or 2)
        const uint32_t stride = count ? descs->size : 0;
        for (uint32_t k = 0; k < count; ++k)
            edits.emplace_back(indices[k], toMaterial((const UnxMaterialDesc*)((const uint8_t*)descs + (size_t)k * stride)));
        find(r)->editMaterials(edits);
    });
}

UNX_API int32_t UNX_CALL UnxSceneAddInstance(UnxRenderer r, const UnxInstanceDesc* d, uint32_t* index)
{
    return call([&] {
        scene::Instance inst = toInstance(d);
        auto h = find(r);
        const uint32_t i = h->add(h->scene().instances, std::move(inst));
        if (index) *index = i;
    });
}

UNX_API int32_t UNX_CALL UnxSceneAddLight(UnxRenderer r, const UnxLightDesc* d, uint32_t* index)
{
    return call([&] {
        // version 2 (v1.93: rayEndBias) or 1 (the same size; that field was reserved - not read: no end bias of its own)
        if (!d) fail("UnxLightDesc is null");
        if (d->size != sizeof(UnxLightDesc) || (d->version != 1 && d->version != 2))
            fail("UnxLightDesc ABI mismatch: size %u version %u, native %zu version 2 (or 1)", d->size, d->version, sizeof(UnxLightDesc));
        if (d->type > UNX_LIGHT_TUBE) fail("unknown light type %u", d->type);
        scene::Light l;
        l.type = (scene::LightType)d->type;
        l.position = f3(d->position);
        l.forward = f3(d->forward);
        l.right = f3(d->right);
        l.color = f3(d->color);
        l.intensity = d->intensity;
        l.range = d->range;
        l.spotInner = d->spotInner;
        l.spotOuter = d->spotOuter;
        l.size = { d->areaSize[0], d->areaSize[1] };
        l.castShadow = d->castShadow != 0;
        l.rayEndBias = d->version >= 2 && d->rayEndBias >= 0 ? d->rayEndBias : -1.0f;
        auto h = find(r);
        const uint32_t i = h->add(h->scene().lights, std::move(l));
        if (index) *index = i;
    });
}

UNX_API int32_t UNX_CALL UnxEnvironmentDefaults(UnxEnvironmentDesc* d)
{
    return call([&] {
        if (!d) fail("UnxEnvironmentDesc output is null");
        const scene::Sun sun;
        const scene::Atmosphere a;
        const scene::Scene s;
        std::memset(d, 0, sizeof *d);
        d->size = sizeof *d;
        d->version = 1;
        auto put = [](float* dst, float3 v) {
            dst[0] = v.x;
            dst[1] = v.y;
            dst[2] = v.z;
        };
        put(d->sunDirection, sun.direction);
        d->sunIlluminance = sun.illuminance;
        put(d->sunColor, sun.color);
        d->sunAngularRadius = sun.angularRadius;
        put(d->windDirection, s.windDirection);
        d->windSpeed = s.windSpeed;
        d->bottomRadius = a.bottomRadius;
        d->topRadius = a.topRadius;
        d->rayleighScaleHeight = a.rayleighScaleHeight;
        d->mieScaleHeight = a.mieScaleHeight;
        put(d->rayleighScattering, a.rayleighScattering);
        d->mieG = a.mieG;
        put(d->mieScattering, a.mieScattering);
        d->ozoneCenter = a.ozoneCenter;
        put(d->mieAbsorption, a.mieAbsorption);
        d->ozoneWidth = a.ozoneWidth;
        put(d->ozoneAbsorption, a.ozoneAbsorption);
        put(d->groundAlbedo, a.groundAlbedo);
    });
}

static scene::Sun sunOf(const UnxEnvironmentDesc* d)
{
    scene::Sun sun;
    sun.direction = f3(d->sunDirection);
    sun.illuminance = d->sunIlluminance;
    sun.color = f3(d->sunColor);
    sun.angularRadius = d->sunAngularRadius;
    return sun;
}

static scene::Atmosphere atmosphereOf(const UnxEnvironmentDesc* d)
{
    scene::Atmosphere a;
    a.bottomRadius = d->bottomRadius;
    a.topRadius = d->topRadius;
    a.rayleighScaleHeight = d->rayleighScaleHeight;
    a.mieScaleHeight = d->mieScaleHeight;
    a.rayleighScattering = f3(d->rayleighScattering);
    a.mieG = d->mieG;
    a.mieScattering = f3(d->mieScattering);
    a.ozoneCenter = d->ozoneCenter;
    a.mieAbsorption = f3(d->mieAbsorption);
    a.ozoneWidth = d->ozoneWidth;
    a.ozoneAbsorption = f3(d->ozoneAbsorption);
    a.groundAlbedo = f3(d->groundAlbedo);
    return a;
}

UNX_API int32_t UNX_CALL UnxSceneSetEnvironment(UnxRenderer r, const UnxEnvironmentDesc* d)
{
    return call([&] {
        requireStruct(d, "UnxEnvironmentDesc");
        auto h = find(r);
        if (h->committed()) fail("the scene is committed; the environment is set before UnxSceneCommit (per frame: UnxFrameSetEnvironment)");
        scene::Scene& s = h->scene();
        s.sun = sunOf(d);
        s.atmosphere = atmosphereOf(d);
        s.windDirection = f3(d->windDirection);
        s.windSpeed = d->windSpeed;
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetEnvironment(UnxRenderer r, const UnxEnvironmentDesc* d)
{
    return call([&] {
        requireStruct(d, "UnxEnvironmentDesc");
        auto h = find(r);
        if (!h->committed()) fail("UnxFrameSetEnvironment changes a committed scene; before UnxSceneCommit use UnxSceneSetEnvironment");
        const scene::Sun sun = sunOf(d);
        const float len = length(sun.direction);
        if (std::abs(len - 1.0f) > 1e-3f) fail("sun direction is not unit length (%f)", len);
        // Wind changes after commit follow INTERFACES 6.4 v1.23: the wind model is memoryless, and the tracks judge a
        // change by its endpoints (windChangeBound); the scene wind of the frame is all they need.
        const float3 wind = f3(d->windDirection);
        if (!(d->windSpeed >= 0) || (d->windSpeed > 0 && std::abs(length(wind) - 1.0f) > 1e-3f))
            fail("wind needs a speed >= 0 and a unit direction (speed %f, |direction| %f)", d->windSpeed, length(wind));
        h->setEnvironment(sun, atmosphereOf(d), FramePacket::Wind{ wind, d->windSpeed });
    });
}

UNX_API int32_t UNX_CALL UnxSceneCommit(UnxRenderer r, UnxSceneInfo* info)
{
    return call([&] {
        auto h = find(r);
        const SceneCommitInfo c = h->commit();
        if (info)
        {
            requireStruct(info, "UnxSceneInfo");
            const scene::Scene& s = h->scene();
            info->textures = (uint32_t)s.textures.size();
            info->materials = (uint32_t)s.materials.size();
            info->meshes = (uint32_t)s.meshes.size();
            info->instances = (uint32_t)s.instances.size();
            info->lights = (uint32_t)s.lights.size();
            info->skeletons = (uint32_t)s.skeletons.size();
            info->triangles = c.triangles;
            info->clusters = c.clusters;
            info->buildMs = c.buildMs;
            std::memset(info->contentHash, 0, sizeof info->contentHash);
            std::memcpy(info->contentHash, c.contentHash.data(), std::min(c.contentHash.size(), sizeof info->contentHash - 1));
        }
    });
}

UNX_API int32_t UNX_CALL UnxSceneContentHash(UnxRenderer r, char hash[65])
{
    return call([&] {
        if (!hash) fail("hash output is null");
        const std::string h = scene::contentHash(find(r)->scene());
        if (h.size() != 64) fail("unexpected content hash length %zu", h.size());
        std::memcpy(hash, h.c_str(), 65);
    });
}

UNX_API int32_t UNX_CALL UnxSceneSave(UnxRenderer r, const char* utf8Path, const char* utf8Name, const UnxCameraDesc* camera)
{
    return call([&] {
        if (!utf8Path || !*utf8Path) fail("scene path is empty");
        const auto h = find(r);
        scene::Scene copy = h->committed() ? h->currentScene() : h->scene();
        copy.name = utf8Name ? utf8Name : "";
        if (camera)
        {
            scene::Camera c;
            c.name = "host";
            c.position = f3(camera->position);
            c.forward = f3(camera->forward);
            c.up = f3(camera->up);
            c.verticalFov = camera->verticalFov;
            c.nearPlane = camera->nearPlane;
            c.ev100 = camera->ev100;
            copy.cameras.insert(copy.cameras.begin(), c);
        }
        const std::string path(utf8Path);
        scene::save(copy, std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())));
        logf("UnravelNext: saved scene '%s' (%zu meshes, %zu instances) to %s, hash %s\n", copy.name.c_str(), copy.meshes.size(), copy.instances.size(), path.c_str(),
             scene::contentHash(copy).substr(0, 16).c_str());
    });
}

UNX_API int32_t UNX_CALL UnxSceneLoad(UnxRenderer r, const char* utf8Path, UnxCameraDesc* camera0, uint32_t* instances, uint32_t* skeletons)
{
    return call([&] {
        if (!utf8Path || !*utf8Path) fail("scene path is empty");
        const auto h = find(r);
        if (h->committed()) fail("UnxSceneLoad after UnxSceneCommit");
        const scene::Scene& now = h->scene();
        if (!now.meshes.empty() || !now.materials.empty() || !now.textures.empty() || !now.instances.empty() || !now.skeletons.empty() || !now.lights.empty())
            fail("UnxSceneLoad needs an empty scene (content was already added)");
        const std::string path(utf8Path);
        scene::Scene loaded = scene::load(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())));
        if (camera0)
        {
            if (loaded.cameras.empty()) fail("%s has no camera", path.c_str());
            const scene::Camera& c = loaded.cameras[0];
            const float v[12] = { c.position.x, c.position.y, c.position.z, c.verticalFov, c.forward.x, c.forward.y, c.forward.z, c.nearPlane,
                                  c.up.x, c.up.y, c.up.z, c.ev100 };
            std::memcpy(camera0, v, sizeof v);
        }
        if (instances) *instances = (uint32_t)loaded.instances.size();
        if (skeletons) *skeletons = (uint32_t)loaded.skeletons.size();
        logf("UnravelNext: loaded scene '%s' (%zu meshes, %zu instances, %zu skeletons) from %s, hash %s\n", loaded.name.c_str(), loaded.meshes.size(),
             loaded.instances.size(), loaded.skeletons.size(), path.c_str(), scene::contentHash(loaded).substr(0, 16).c_str());
        h->scene() = std::move(loaded);
    });
}

UNX_API int32_t UNX_CALL UnxFrameQueue(UnxRenderer r, const UnxFrameDesc* d, uint64_t* ticket)
{
    return call([&] {
        requireStruct(d, "UnxFrameDesc");
        if (!ticket) fail("ticket output is null");
        FramePacket p;
        p.frameIndex = d->frameIndex;
        p.time = d->time;
        p.deltaTime = d->deltaTime;
        p.width = d->outputWidth;
        p.height = d->outputHeight;
        p.camera.position = f3(d->camera.position);
        p.camera.forward = f3(d->camera.forward);
        p.camera.up = f3(d->camera.up);
        p.camera.verticalFov = d->camera.verticalFov;
        p.camera.nearPlane = d->camera.nearPlane;
        p.camera.ev100 = d->camera.ev100;
        p.output = reinterpret_cast<ID3D12Resource*>(d->output);
        p.displayPeak = d->displayPeak;
        if (!(p.displayPeak == 0 || (std::isfinite(p.displayPeak) && p.displayPeak >= 1)))
            fail("displayPeak %g: 0 (SDR) or the HDR display's peak over paper white (>= 1)", p.displayPeak);
        *ticket = globalTicket(r, find(r)->queueFrame(p));
    });
}

UNX_API int32_t UNX_CALL UnxFrameRenderStandalone(UnxRenderer r, uint64_t ticket, void* readback, uint64_t readbackBytes)
{
    return call([&] {
        if (ticketRenderer(ticket) != r) fail("ticket %llu belongs to renderer %llu, not %llu", (unsigned long long)ticket, (unsigned long long)ticketRenderer(ticket), (unsigned long long)r);
        find(r)->renderStandalone(ticketLocal(ticket), readback, (size_t)readbackBytes);
    });
}

UNX_API int32_t UNX_CALL UnxFramePassTimingsLatest(UnxRenderer r, UnxPassTiming* passes, uint32_t capacity, uint32_t* count)
{
    return call([&] {
        if (!count || (capacity && !passes)) fail("pass timing output is null");
        const FrameStats s = find(r)->latestStats();
        *count = (uint32_t)s.passMs.size();
        for (uint32_t i = 0; i < capacity && i < s.passMs.size(); ++i)
        {
            std::memset(passes[i].name, 0, sizeof passes[i].name);
            std::memcpy(passes[i].name, s.passMs[i].first.data(), std::min(s.passMs[i].first.size(), sizeof passes[i].name - 1));
            passes[i].ms = s.passMs[i].second;
        }
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetTransforms(UnxRenderer r, const UnxTransformUpdate* updates, uint32_t count)
{
    return call([&] {
        if (count && !updates) fail("updates is null");
        std::vector<render::InstanceTransformUpdate> u(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            u[i].instance = updates[i].instance;
            u[i].objectToWorld = affine(updates[i].transform);
            if (updates[i].flags & ~(uint32_t)UNX_TRANSFORM_TELEPORT) fail("transform update %u: unknown flags 0x%x", i, updates[i].flags);
            u[i].flags = (updates[i].flags & UNX_TRANSFORM_TELEPORT) ? render::kTransformTeleport : 0u;
        }
        find(r)->setTransforms(u);
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetSkeleton(UnxRenderer r, uint32_t skeleton, const float* jointToModel, uint32_t jointCount)
{
    return call([&] {
        if (!jointToModel || !jointCount) fail("skeleton pose is empty");
        std::vector<float3x4> joints(jointCount);
        for (uint32_t j = 0; j < jointCount; ++j) joints[j] = affine(jointToModel + 12ull * j);
        find(r)->setSkeleton(skeleton, std::move(joints));
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetSkeletons(UnxRenderer r, uint32_t count, const uint32_t* skeletons, const float* jointToModel, uint64_t jointCount)
{
    return call([&] {
        if (count && (!skeletons || !jointToModel)) fail("skeleton list or poses are null");
        const auto h = find(r);
        // Validate the whole batch before any update is recorded (a failed call changes nothing).
        uint64_t total = 0;
        for (uint32_t i = 0; i < count; ++i) total += h->jointCount(skeletons[i]);
        if (total != jointCount) fail("%u skeletons hold %llu joints, the pose buffer %llu", count, (unsigned long long)total, (unsigned long long)jointCount);
        const float* at = jointToModel;
        for (uint32_t i = 0; i < count; ++i)
        {
            std::vector<float3x4> joints(h->jointCount(skeletons[i]));
            for (float3x4& j : joints)
            {
                j = affine(at);
                at += 12;
            }
            h->setSkeleton(skeletons[i], std::move(joints));
        }
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetInstanceVisible(UnxRenderer r, uint32_t instance, uint32_t visible)
{
    return call([&] { find(r)->setInstanceVisible(instance, visible != 0); });
}

UNX_API int32_t UNX_CALL UnxSceneAddBlendShape(UnxRenderer r, uint32_t mesh, const char* name, uint32_t vertexCount, const uint32_t* vertices,
                                               const float* deltaPositions, const float* deltaNormals)
{
    return call([&] {
        if (vertexCount && (!vertices || !deltaPositions)) fail("blend shape: vertices or position offsets are null");
        scene::BlendShape b;
        b.name = name ? name : "";
        b.vertices.assign(vertices, vertices + vertexCount);
        for (uint32_t v = 0; v < vertexCount; ++v) b.deltaPositions.push_back(f3(deltaPositions + 3ull * v));
        if (deltaNormals)
            for (uint32_t v = 0; v < vertexCount; ++v) b.deltaNormals.push_back(f3(deltaNormals + 3ull * v));
        find(r)->addBlendShape(mesh, std::move(b));
    });
}

UNX_API int32_t UNX_CALL UnxSceneSetVertexAnimation(UnxRenderer r, uint32_t mesh, float framesPerSecond, uint32_t frameCount, uint32_t loop,
                                                    const float* positions, const float* normals)
{
    return call([&] {
        const auto h = find(r);
        const uint32_t n = h->meshVertexCount(mesh);
        if (!(framesPerSecond > 0) || frameCount == 0 || !positions) fail("vertex animation: rate, frame count and positions are required");
        scene::VertexAnimation a;
        a.framesPerSecond = framesPerSecond;
        a.frameCount = frameCount;
        a.loop = loop != 0;
        const uint64_t count = (uint64_t)frameCount * n;
        for (uint64_t k = 0; k < count; ++k) a.positions.push_back(f3(positions + 3 * k));
        if (normals)
            for (uint64_t k = 0; k < count; ++k) a.normals.push_back(f3(normals + 3 * k));
        h->setVertexAnimation(mesh, std::move(a));
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetMorphs(UnxRenderer r, uint32_t count, const uint32_t* instances, const float* weights, uint64_t weightCount, const float* times)
{
    return call([&] {
        if (count && !instances) fail("morphs: instance list is null");
        const auto h = find(r);
        uint64_t total = 0;
        for (uint32_t i = 0; i < count; ++i) total += h->blendShapeCount(instances[i]);
        if (total != weightCount) fail("%u instances hold %llu blend shapes, the weight buffer %llu", count, (unsigned long long)total, (unsigned long long)weightCount);
        if (weightCount && !weights) fail("morphs: weights are null");
        const float* at = weights;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t n = h->blendShapeCount(instances[i]);
            h->setMorph(instances[i], std::vector<float>(at, at + n), times ? times[i] : 0.0f);
            at += n;
        }
    });
}

UNX_API int32_t UNX_CALL UnxSceneReserveRuntime(UnxRenderer r, const UnxRuntimeCapacity* c)
{
    return call([&] {
        requireStruct(c, "UnxRuntimeCapacity");
        render::RuntimeCapacity rc;
        rc.meshes = c->meshes, rc.submeshes = c->submeshes, rc.vertices = c->vertices, rc.indices = c->indices, rc.clusters = c->clusters;
        rc.clusterVertexIndices = c->clusterVertexIndices, rc.clusterTriangles = c->clusterTriangles, rc.nodes = c->nodes, rc.instances = c->instances;
        find(r)->reserveRuntime(rc);
    });
}

UNX_API int32_t UNX_CALL UnxFrameAddRuntimeMesh(UnxRenderer r, const UnxMeshDesc* d, uint32_t* id)
{
    return call([&] {
        scene::Mesh m = meshFromDesc(d);
        const uint32_t k = find(r)->addRuntimeMesh(std::move(m));
        if (id) *id = k;
    });
}

UNX_API int32_t UNX_CALL UnxFrameRemoveRuntimeMesh(UnxRenderer r, uint32_t id)
{
    return call([&] { find(r)->removeRuntimeMesh(id); });
}

UNX_API int32_t UNX_CALL UnxFrameAddRuntimeInstance(UnxRenderer r, uint32_t mesh, const float objectToWorld[12], uint32_t flags, uint32_t* id)
{
    return call([&] {
        if (!objectToWorld) fail("runtime instance: transform is null");
        const uint32_t k = find(r)->addRuntimeInstance(mesh, affine(objectToWorld), flags);
        if (id) *id = k;
    });
}

UNX_API int32_t UNX_CALL UnxFrameRemoveRuntimeInstance(UnxRenderer r, uint32_t id)
{
    return call([&] { find(r)->removeRuntimeInstance(id); });
}

UNX_API int32_t UNX_CALL UnxFrameSetRuntimeTransforms(UnxRenderer r, uint32_t count, const uint32_t* ids, const float* objectToWorld)
{
    return call([&] {
        if (count && (!ids || !objectToWorld)) fail("runtime transforms: ids or transforms are null");
        const auto h = find(r);
        for (uint32_t k = 0; k < count; ++k) h->setRuntimeTransform(ids[k], affine(objectToWorld + 12ull * k));
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetTerrainDeformation(UnxRenderer r, const UnxTerrainDeformation* d)
{
    return call([&] {
        requireStruct(d, "UnxTerrainDeformation");
        if (d->tileCount && !d->tiles) fail("terrain deformation: tiles is null");
        find(r)->setTerrainDeformation(d->originX, d->originZ, d->spacing, d->texels, d->heights, { d->tiles, d->tileCount });
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetOriginShift(UnxRenderer r, const double shift[3])
{
    return call([&] {
        if (!shift) fail("origin shift is null");
        find(r)->setOriginShift({ (float)shift[0], (float)shift[1], (float)shift[2] });
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetDiscontinuity(UnxRenderer r, uint32_t flags)
{
    static_assert(UNX_DISCONTINUITY_RESTORE == render::kDiscontinuityRestore && UNX_DISCONTINUITY_CUT == render::kDiscontinuityCut);
    return call([&] { find(r)->setDiscontinuity(flags); });
}

UNX_API int32_t UNX_CALL UnxFrameSetSimulation(UnxRenderer r, uint32_t gpuSimulation)
{
    static_assert(UNX_GPU_SIMULATION_SOFT == render::kGpuSimulationSoft && UNX_GPU_SIMULATION_VFX == render::kGpuSimulationVfx &&
                  UNX_GPU_SIMULATION_RIGID == render::kGpuSimulationRigid);
    return call([&] { find(r)->setSimulation(gpuSimulation); });
}

UNX_API int32_t UNX_CALL UnxFrameSetSun(UnxRenderer r, const float direction[3], float illuminance, const float color[3], float angularRadius)
{
    return call([&] {
        if (!direction || !color) fail("sun direction or color is null");
        scene::Sun s;
        s.direction = f3(direction);
        s.illuminance = illuminance;
        s.color = f3(color);
        s.angularRadius = angularRadius;
        const float len = std::sqrt(s.direction.x * s.direction.x + s.direction.y * s.direction.y + s.direction.z * s.direction.z);
        if (std::abs(len - 1.0f) > 1e-3f) fail("sun direction is not unit length (%f)", len);
        find(r)->setSun(s);
    });
}

namespace unx::host::plugin
{
void destroyDeviceRenderers()
{
    std::vector<std::shared_ptr<HostRenderer>> doomed;
    {
        std::lock_guard lock(g_renderersMutex);
        for (auto it = g_renderers.begin(); it != g_renderers.end();)
        {
            if (!it->second->options().standalone)
            {
                doomed.push_back(std::move(it->second));
                it = g_renderers.erase(it);
            }
            else
                ++it;
        }
    }
    if (!doomed.empty()) logf("UnravelNext: Unity's device is going away; destroying %zu renderer(s) bound to it\n", doomed.size());
    doomed.clear();  // waits for the GPU while the device still exists
}

// UNX_EVENT_RENDER on Unity's submission thread (queue-access event, Unity flushed its command buffers first). Each of
// the frame's lists goes through Unity's ExecuteCommandList with the output texture declared UNORDERED_ACCESS before and
// after, so Unity's state tracker transitions it for the list and knows its state for the following blit.
void renderEvent(uint64_t ticket)
{
    static std::atomic<uint32_t> failures{ 0 };
    try
    {
        IUnityGraphicsD3D12v8* d3d = unityD3D12();
        if (!d3d || render::deviceWasRemoved()) return;  // after a removal the main thread's next call reports UNX_DEVICE_REMOVED
        std::shared_ptr<HostRenderer> renderer = find(ticketRenderer(ticket));
        renderer->renderOnHost(ticketLocal(ticket), [d3d](ID3D12CommandList* list, ID3D12Resource* output) {
            ComPtr<ID3D12GraphicsCommandList> graphics;
            check(list->QueryInterface(IID_PPV_ARGS(&graphics)), "frame list is not a graphics command list");
            UnityGraphicsD3D12ResourceState state{ output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            d3d->ExecuteCommandList(graphics.Get(), 1, &state);
        });
    }
    catch (const std::exception& e)
    {
        // Every failure is reported; the message of the latest one stays in UnxLastError for the managed side.
        const uint32_t n = ++failures;
        failWith(format("UNX_EVENT_RENDER failure %u: %s", n, e.what()).c_str());
    }
}
} // namespace unx::host::plugin

UNX_API int32_t UNX_CALL UnxFrameGraphStatsLatest(UnxRenderer r, UnxFrameGraphStats* stats)
{
    return call([&] {
        if (!stats) fail("UnxFrameGraphStats is null");
        const bool v2 = stats->size == sizeof(UnxFrameGraphStats) && stats->version == 2;
        if (!v2 && !(stats->size == offsetof(UnxFrameGraphStats, queues) && stats->version == 1))
            fail("UnxFrameGraphStats ABI mismatch: size %u version %u, native %zu version 2 (or %zu version 1)", stats->size, stats->version, sizeof(UnxFrameGraphStats),
                 offsetof(UnxFrameGraphStats, queues));
        const FrameStats s = find(r)->latestStats();
        const GraphFrameStats& g = s.graph;
        if (v2)
            for (uint32_t q = 0; q < 2; ++q)
                stats->queues[q] = { s.queues[q].lists, 0, s.queues[q].headMs, s.queues[q].tailMs, s.queues[q].gapMs };
        stats->frameIndex = s.frameIndex;
        stats->livePasses = g.livePasses;
        stats->commandLists = g.commandLists;
        stats->barrierBatches = g.barrierBatches;
        stats->barriers = g.barriers;
        stats->crossQueueSyncs = g.crossQueueSyncs;
        stats->transientResources = g.transientResources;
        stats->planReused = g.planReused ? 1u : 0u;
        stats->sceneRevision = g.sceneRevision;
        stats->transientBytesAliased = g.transientBytesAliased;
        stats->cpuCompileMs = g.cpuCompileMs;
    });
}

UNX_API int32_t UNX_CALL UnxVideoMemory(UnxRenderer r, UnxVideoMemoryInfo* info)
{
    return call([&] {
        requireStruct(info, "UnxVideoMemoryInfo");
        ID3D12Device* device = nullptr;
        if (r) device = find(r)->d3dDevice();
        else if (IUnityGraphicsD3D12v8* unity = plugin::unityD3D12()) device = unity->GetDevice();
        if (!device) fail("UnxVideoMemory: no device (renderer 0 outside Unity)");
        const LUID luid = device->GetAdapterLuid();
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) fail("UnxVideoMemory: CreateDXGIFactory2 failed");
        ComPtr<IDXGIAdapter3> adapter;
        if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) fail("UnxVideoMemory: the device's adapter was not found");
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
        if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
            FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal)))
            fail("UnxVideoMemory: QueryVideoMemoryInfo failed");
        info->localBudget = local.Budget;
        info->localUsage = local.CurrentUsage;
        info->localReservation = local.CurrentReservation;
        info->localAvailableForReservation = local.AvailableForReservation;
        info->nonLocalBudget = nonLocal.Budget;
        info->nonLocalUsage = nonLocal.CurrentUsage;
        info->nonLocalReservation = nonLocal.CurrentReservation;
        info->nonLocalAvailableForReservation = nonLocal.AvailableForReservation;
    });
}

UNX_API int32_t UNX_CALL UnxFrameStatsLatest(UnxRenderer r, UnxFrameStats* stats)
{
    return call([&] {
        requireStruct(stats, "UnxFrameStats");
        const FrameStats s = find(r)->latestStats();
        stats->frameIndex = s.frameIndex;
        stats->gpuMs = s.gpuMs;
        stats->cpuRecordMs = s.cpuRecordMs;
        stats->cpuSubmitMs = s.cpuSubmitMs;
        stats->passes = s.passes;
    });
}

// ---- The picture's settings a game changes while it runs: colour grading, post settings, the HDR output's encoding.
UNX_API int32_t UNX_CALL UnxFrameSetColorGrading(UnxRenderer r, const UnxColorGradingDesc* grading)
{
    return call([&] {
        render::ColorGradingDesc g;  // null: the quality file's grading
        if (grading)
        {
            if (grading->size != sizeof(UnxColorGradingDesc) || grading->version != 1)
                fail("UnxFrameSetColorGrading: UnxColorGradingDesc size %u version %u", grading->size, grading->version);
            g.enabled = true;
            g.temperature = grading->temperature;
            g.tint = grading->tint;
            const UnxColorGradingRange* from[4] = { &grading->global, &grading->shadows, &grading->midtones, &grading->highlights };
            render::ColorGradingRange* to[4] = { &g.global, &g.shadows, &g.midtones, &g.highlights };
            for (int i = 0; i < 4; ++i)
                for (int c = 0; c < 4; ++c)
                {
                    to[i]->saturation[c] = from[i]->saturation[c];
                    to[i]->contrast[c] = from[i]->contrast[c];
                    to[i]->gamma[c] = from[i]->gamma[c];
                    to[i]->gain[c] = from[i]->gain[c];
                    to[i]->offset[c] = from[i]->offset[c];
                }
            g.shadowsMax = grading->shadowsMax;
            g.highlightsMin = grading->highlightsMin;
            g.highlightsMax = grading->highlightsMax;
        }
        find(r)->setColorGrading(g);
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetPost(UnxRenderer r, const UnxPostSettingsDesc* post)
{
    return call([&] {
        render::PostSettingsDesc p;  // null: every value the quality file's
        float compensation = 0;
        if (post)
        {
            if (post->size != sizeof(UnxPostSettingsDesc) || post->version != 1)
                fail("UnxFrameSetPost: UnxPostSettingsDesc size %u version %u", post->size, post->version);
            compensation = post->exposureCompensation;
            p.exposureMinEv = post->exposureMinEv100;
            p.exposureMaxEv = post->exposureMaxEv100;
            p.bloomStrength = post->bloomIntensity;
            p.vignette = post->vignette;
            p.motionBlurShutter = post->motionBlurAmount;
            p.diaphragmBlades = post->diaphragmBlades;
            p.lensFullAperture = post->lensFullAperture;
        }
        find(r)->setPost(p, compensation);
    });
}

UNX_API int32_t UNX_CALL UnxFrameSetDisplayEncoding(UnxRenderer r, int32_t encoding, float paperWhiteNits)
{
    return call([&] { find(r)->setDisplayEncoding(encoding, paperWhiteNits); });
}

// ---- Weather: the frame's fog, fog volumes and clouds with what the renderer's frame gained after their first
// descriptions were frozen, the weather record and the lightning flash (UnravelNextHost.h; optional exports within ABI 6).
UNX_API int32_t UNX_CALL UnxFrameSetFog2(UnxRenderer r, const UnxFogDesc2* fog)
{
    return call([&] {
        render::FogDesc f;  // null: the quality file's fog
        if (fog)
        {
            if (fog->size != sizeof(UnxFogDesc2) || fog->version != 1) fail("UnxFrameSetFog2: UnxFogDesc2 size %u version %u", fog->size, fog->version);
            f.enabled = true;
            f.density = fog->density, f.heightFalloff = fog->heightFalloff, f.height = fog->height;
            f.albedo[0] = fog->albedo[0], f.albedo[1] = fog->albedo[1], f.albedo[2] = fog->albedo[2];
            f.phaseG = fog->phaseG, f.startDistance = fog->startDistance, f.skyAmount = fog->skyAmount;
            f.noiseAmount = fog->noiseAmount, f.noiseScale = fog->noiseScale;
            f.density2 = fog->density2, f.heightFalloff2 = fog->heightFalloff2, f.height2 = fog->height2;
        }
        find(r)->setFog(f);
    });
}
UNX_API int32_t UNX_CALL UnxFrameSetFogVolumes2(UnxRenderer r, const UnxFogVolumeDesc2* volumes, uint32_t count)
{
    return call([&] {
        if (count && !volumes) fail("UnxFrameSetFogVolumes2: no volumes");
        std::vector<render::FogVolumeDesc> in(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const UnxFogVolumeDesc2& d = volumes[i];
            if (d.size != sizeof(UnxFogVolumeDesc2) || d.version != 1) fail("UnxFrameSetFogVolumes2: UnxFogVolumeDesc2 %u size %u version %u", i, d.size, d.version);
            render::FogVolumeDesc& v = in[i];
            for (int k = 0; k < 3; ++k) v.centre[k] = d.centre[k], v.halfSize[k] = d.halfSize[k], v.albedo[k] = d.albedo[k], v.gridSize[k] = d.gridSize[k];
            v.yaw = d.yaw, v.shape = d.shape, v.density = d.density, v.heightFalloff = d.heightFalloff, v.edge = d.edge;
            v.sourcePlane = d.sourcePlane, v.riseSpeed = d.riseSpeed, v.turbulence = d.turbulence, v.turbulenceScale = d.turbulenceScale;
            v.grid = d.grid;  // (the caller's texels: setFogVolumes checks the size and copies them)
        }
        find(r)->setFogVolumes(in);
    });
}
UNX_API int32_t UNX_CALL UnxFrameSetClouds2(UnxRenderer r, const UnxCloudDesc2* clouds)
{
    return call([&] {
        render::CloudLayerDesc c;  // null: no clouds (both coverages 0)
        if (clouds)
        {
            if (clouds->size != sizeof(UnxCloudDesc2) || clouds->version != 1) fail("UnxFrameSetClouds2: UnxCloudDesc2 size %u version %u", clouds->size, clouds->version);
            c.coverage = clouds->coverage;
            c.baseAltitude = clouds->baseAltitude, c.topAltitude = clouds->topAltitude;
            c.sigmaMax = clouds->sigmaMax, c.albedo = clouds->albedo;
            c.windX = clouds->windX, c.windZ = clouds->windZ;
            c.seed = clouds->seed;
            c.cirrusCoverage = clouds->cirrusCoverage;
            c.cirrusAltitude = clouds->cirrusAltitude, c.cirrusOpticalDepth = clouds->cirrusOpticalDepth;
            c.cirrusWindX = clouds->cirrusWindX, c.cirrusWindZ = clouds->cirrusWindZ;
        }
        find(r)->setClouds(c);
    });
}
UNX_API int32_t UNX_CALL UnxFrameSetWeather(UnxRenderer r, const UnxWeatherDesc* weather)
{
    return call([&] {
        render::WeatherFrame w;  // null: no weather
        if (weather)
        {
            if (weather->size != sizeof(UnxWeatherDesc) || weather->version != 1)
                fail("UnxFrameSetWeather: UnxWeatherDesc size %u version %u", weather->size, weather->version);
            w.rainRate = weather->rainRate, w.wetness = weather->wetness;
            w.snowRate = weather->snowRate, w.snowDepth = weather->snowDepth;
            w.cloudCover = weather->cloudCover;
            w.rainDirection = { weather->rainDirection[0], weather->rainDirection[1], weather->rainDirection[2] };
        }
        find(r)->setWeather(w);
    });
}
UNX_API int32_t UNX_CALL UnxFrameSetLightning(UnxRenderer r, const UnxLightningDesc* lightning)
{
    return call([&] {
        render::LightningDesc l;  // null: no flash (intensity 0)
        if (lightning)
        {
            if (lightning->size != sizeof(UnxLightningDesc) || lightning->version != 1)
                fail("UnxFrameSetLightning: UnxLightningDesc size %u version %u", lightning->size, lightning->version);
            for (int k = 0; k < 3; ++k) l.position[k] = lightning->position[k], l.color[k] = lightning->color[k];
            l.intensity = lightning->intensity;
            l.radius = lightning->radius;
        }
        find(r)->setLightning(l);
    });
}
UNX_API int32_t UNX_CALL UnxFrameSetPoolWeather(UnxRenderer r, const UnxPoolWeatherDesc* pools, uint32_t count)
{
    return call([&] {
        if (count && !pools) fail("UnxFrameSetPoolWeather: no basins");
        std::vector<HostRenderer::PoolWeather> in(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const UnxPoolWeatherDesc& d = pools[i];
            if (d.size != sizeof(UnxPoolWeatherDesc) || d.version != 1) fail("UnxFrameSetPoolWeather: UnxPoolWeatherDesc %u size %u version %u", i, d.size, d.version);
            in[i] = { d.pool, d.steamDensity, d.steamHeight, d.steamRiseSpeed, d.steamTurbulence, d.rainExposure };
        }
        find(r)->setPoolWeather(in);
    });
}

// ---- Frame pacing data: the GPU frame time, the main view's resolution with the dynamic resolution's state, the GPU
// time by pass group (the pass names' part before the first '.').
UNX_API int32_t UNX_CALL UnxFrameGetStatistics(UnxRenderer r, UnxFrameStatistics* statistics)
{
    return call([&] {
        requireStruct(statistics, "UnxFrameStatistics");
        const auto renderer = find(r);
        const FrameStats s = renderer->latestStats();
        const FramePacing p = renderer->framePacing();
        statistics->frameIndex = s.frameIndex;
        statistics->gpuMs = s.gpuMs;
        statistics->cpuRecordMs = s.cpuRecordMs;
        statistics->cpuSubmitMs = s.cpuSubmitMs;
        statistics->gpuRenderWidth = s.renderWidth;
        statistics->gpuRenderHeight = s.renderHeight;
        statistics->outputWidth = p.outputWidth;
        statistics->outputHeight = p.outputHeight;
        statistics->renderWidth = p.renderWidth;
        statistics->renderHeight = p.renderHeight;
        statistics->dynamicResolution = p.dynamicResolution ? 1u : 0u;
        statistics->resolutionScale = p.outputHeight ? (float)p.renderHeight / (float)p.outputHeight : 1.0f;
        statistics->dynamicTargetMs = p.targetMs;
        statistics->dynamicMeasuredMs = p.controllerMs;
        statistics->minRenderHeight = p.minRenderHeight;
        statistics->maxRenderHeight = p.maxRenderHeight;
        statistics->passes = s.passes;
        // the groups, the largest time first; the ones beyond the table summed into its last entry
        struct Group
        {
            std::string name;
            double ms = 0;
            uint32_t passes = 0;
        };
        std::vector<Group> groups;
        for (const auto& [name, ms] : s.passMs)
        {
            const std::string prefix = name.substr(0, name.find('.'));
            auto at = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.name == prefix; });
            if (at == groups.end()) at = groups.insert(groups.end(), Group{ prefix });
            at->ms += ms;
            ++at->passes;
        }
        std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.ms > b.ms; });
        if (groups.size() > UNX_FRAME_STATISTICS_GROUPS)
        {
            Group other{ "other" };
            for (size_t i = UNX_FRAME_STATISTICS_GROUPS - 1; i < groups.size(); ++i)
            {
                other.ms += groups[i].ms;
                other.passes += groups[i].passes;
            }
            groups.resize(UNX_FRAME_STATISTICS_GROUPS - 1);
            groups.push_back(other);
        }
        statistics->groupCount = (uint32_t)groups.size();
        std::memset(statistics->groups, 0, sizeof statistics->groups);
        for (size_t i = 0; i < groups.size(); ++i)
        {
            UnxFrameStatisticsGroup& out = statistics->groups[i];
            std::memcpy(out.name, groups[i].name.data(), std::min(groups[i].name.size(), sizeof out.name - 1));
            out.gpuMs = (float)groups[i].ms;
            out.passes = groups[i].passes;
        }
    });
}

// ---- Sprite looks, a decal's extra fields, instances that take no decals (UnravelNextHost.h's last block) ---------------
UNX_API int32_t UNX_CALL UnxSpriteLookDefaults(UnxSpriteLookDesc* d)
{
    return call([&] {
        if (!d) fail("UnxSpriteLookDesc output is null");
        const fx::SpriteLook l;
        std::memset(d, 0, sizeof *d);
        d->size = sizeof *d;
        d->version = 1;
        d->texture = d->normalTexture = d->motionTexture = UNX_NONE;
        d->motionScale = l.motionScale;
        d->blend = (uint32_t)l.blend;
        d->facing = (uint32_t)l.facing;
        d->normal = (uint32_t)l.normal;
        d->flags = (l.frameBlend ? UNX_SPRITE_LOOK_FRAME_BLEND : 0u) | (l.framesOverLife ? UNX_SPRITE_LOOK_FRAMES_OVER_LIFE : 0u) | (l.lit ? UNX_SPRITE_LOOK_LIT : 0u) |
                   (l.smooth ? UNX_SPRITE_LOOK_SMOOTH : 0u) | (l.castShadow ? UNX_SPRITE_LOOK_CAST_SHADOW : 0u) |
                   (l.ribbonUv == fx::RibbonUv::Age ? UNX_SPRITE_LOOK_RIBBON_UV_AGE : 0u);
        d->axis[0] = l.axis.x, d->axis[1] = l.axis.y, d->axis[2] = l.axis.z;
        d->aspect = l.aspect;
        d->rotationRate = l.rotationRate;
        d->stretch = l.stretch;
        d->stretchMax = l.stretchMax;
        d->pivot[0] = l.pivot.x, d->pivot[1] = l.pivot.y;
        d->shadowDensity = l.shadowDensity;
    });
}

UNX_API int32_t UNX_CALL UnxVfxSetSpriteLook(UnxRenderer r, uint32_t index, const UnxSpriteLookDesc* d)
{
    return call([&] {
        if (!d)
        {
            find(r)->setSpriteLook(index, nullptr);
            return;
        }
        if (d->size != sizeof(UnxSpriteLookDesc) || d->version != 1) fail("UnxVfxSetSpriteLook: UnxSpriteLookDesc size %u version %u", d->size, d->version);
        if (d->reserved[0] != 0 || d->reserved[1] != 0) fail("UnxVfxSetSpriteLook: reserved is not 0");
        if (d->blend > UNX_SPRITE_BLEND_PREMULTIPLIED || d->facing > UNX_SPRITE_FACING_AXIS || d->normal > UNX_SPRITE_NORMAL_MAP)
            fail("UnxVfxSetSpriteLook: blend %u, facing %u, normal %u", d->blend, d->facing, d->normal);
        const uint32_t known = UNX_SPRITE_LOOK_FRAME_BLEND | UNX_SPRITE_LOOK_FRAMES_OVER_LIFE | UNX_SPRITE_LOOK_LIT | UNX_SPRITE_LOOK_SMOOTH |
                               UNX_SPRITE_LOOK_CAST_SHADOW | UNX_SPRITE_LOOK_RIBBON_UV_AGE;
        if ((d->flags & ~known) != 0) fail("UnxVfxSetSpriteLook: unknown flags 0x%x", d->flags);
        fx::SpriteLook l;
        l.texture = d->texture;
        l.normalTexture = d->normalTexture;
        l.motionTexture = d->motionTexture;
        l.motionScale = d->motionScale;
        l.blend = (fx::SpriteBlend)d->blend;
        l.facing = (fx::SpriteFacing)d->facing;
        l.normal = (fx::SpriteNormal)d->normal;
        l.frameBlend = (d->flags & UNX_SPRITE_LOOK_FRAME_BLEND) != 0;
        l.framesOverLife = (d->flags & UNX_SPRITE_LOOK_FRAMES_OVER_LIFE) != 0;
        l.lit = (d->flags & UNX_SPRITE_LOOK_LIT) != 0;
        l.smooth = (d->flags & UNX_SPRITE_LOOK_SMOOTH) != 0;
        l.castShadow = (d->flags & UNX_SPRITE_LOOK_CAST_SHADOW) != 0;
        l.ribbonUv = (d->flags & UNX_SPRITE_LOOK_RIBBON_UV_AGE) != 0 ? fx::RibbonUv::Age : fx::RibbonUv::Distance;
        l.axis = { d->axis[0], d->axis[1], d->axis[2] };
        l.aspect = d->aspect;
        l.rotationRate = d->rotationRate;
        l.stretch = d->stretch;
        l.stretchMax = d->stretchMax;
        l.pivot = { d->pivot[0], d->pivot[1] };
        l.shadowDensity = d->shadowDensity;
        find(r)->setSpriteLook(index, &l);
    });
}

UNX_API int32_t UNX_CALL UnxDecalExtraDefaults(UnxDecalExtraDesc* d)
{
    return call([&] {
        if (!d) fail("UnxDecalExtraDesc output is null");
        const decal::Decal e;
        std::memset(d, 0, sizeof *d);
        d->size = sizeof *d;
        d->version = 1;
        d->color[0] = e.color.x, d->color[1] = e.color.y, d->color[2] = e.color.z;
        d->channels = e.channels;
        d->blend = (uint32_t)e.blend;
        d->emissive = e.emissive;
        d->fadeScreenSize = e.fadeScreenSize;
        d->fadeInStart = e.fadeInStart, d->fadeInDuration = e.fadeInDuration;
        d->fadeOutStart = e.fadeOutStart, d->fadeOutDuration = e.fadeOutDuration;
    });
}

UNX_API int32_t UNX_CALL UnxDecalSetExtra(UnxRenderer r, uint32_t id, const UnxDecalExtraDesc* d)
{
    return call([&] {
        if (!d) fail("UnxDecalSetExtra: no description");
        if (d->size != sizeof(UnxDecalExtraDesc) || d->version != 1) fail("UnxDecalSetExtra: UnxDecalExtraDesc size %u version %u", d->size, d->version);
        if (d->reserved != 0) fail("UnxDecalSetExtra: reserved is not 0");
        decal::Decal e;
        e.color = { d->color[0], d->color[1], d->color[2] };
        e.channels = d->channels;
        e.blend = (decal::DecalBlend)d->blend;
        e.emissive = d->emissive;
        e.fadeScreenSize = d->fadeScreenSize;
        e.fadeInStart = d->fadeInStart, e.fadeInDuration = d->fadeInDuration;
        e.fadeOutStart = d->fadeOutStart, e.fadeOutDuration = d->fadeOutDuration;
        find(r)->decalSetExtra(id, e);
    });
}

UNX_API int32_t UNX_CALL UnxSceneSetInstanceReceivesDecals(UnxRenderer r, uint32_t instance, int32_t receives)
{
    return call([&] { find(r)->setInstanceReceivesDecals(instance, receives != 0); });
}
UNX_API int32_t UNX_CALL UnxFrameSetPoolFlow(UnxRenderer r, const UnxPoolFlowDesc* pools, uint32_t count)
{
    return call([&] {
        if (count && !pools) fail("UnxFrameSetPoolFlow: no basins");
        std::vector<HostRenderer::PoolFlow> in(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const UnxPoolFlowDesc& d = pools[i];
            if (d.size != sizeof(UnxPoolFlowDesc) || d.version != 1) fail("UnxFrameSetPoolFlow: UnxPoolFlowDesc %u size %u version %u", i, d.size, d.version);
            HostRenderer::PoolFlow& f = in[i];
            f.id = d.pool;
            f.velocity[0] = d.velocity[0], f.velocity[1] = d.velocity[1];
            f.map = d.map, f.mapSpeed = d.mapSpeed;
            f.drain[0] = d.drain[0], f.drain[1] = d.drain[1];
            f.drainInflow = d.drainInflow, f.drainCirculation = d.drainCirculation;
            f.waveLength = d.waveLength, f.waveSlope = d.waveSlope;
        }
        find(r)->setPoolFlow(in);
    });
}
