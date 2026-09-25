// UnravelNext.dll renderer C ABI (UnravelNextHost.h): converts the fixed-size ABI structs into scene:: types and
// forwards to HostRenderer. Every entry point catches exceptions and reports them through UnxLastError.
#include "unx/host/UnravelNextHost.h"

#include "Renderer/HostRenderer.h"
#include "Unity/PluginState.h"

#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D12.h"
#include "IUnityInterface.h"

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

template <typename F>
int32_t call(F&& f)
{
    try
    {
        f();
        return UNX_OK;
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
        r.reset();  // waits for the GPU (HostRenderer destructor)
    });
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

UNX_API int32_t UNX_CALL UnxSceneAddMaterial(UnxRenderer r, const UnxMaterialDesc* d, uint32_t* index)
{
    return call([&] {
        requireStruct(d, "UnxMaterialDesc");
        if (d->materialClass > UNX_MATERIAL_SUBSURFACE) fail("unknown material class %u", d->materialClass);
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
        auto h = find(r);
        const uint32_t i = h->add(h->scene().materials, std::move(m));
        if (index) *index = i;
    });
}

UNX_API int32_t UNX_CALL UnxSceneAddMesh(UnxRenderer r, const UnxMeshDesc* d, uint32_t* index)
{
    return call([&] {
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

UNX_API int32_t UNX_CALL UnxSceneAddInstance(UnxRenderer r, const UnxInstanceDesc* d, uint32_t* index)
{
    return call([&] {
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
        auto h = find(r);
        const uint32_t i = h->add(h->scene().instances, std::move(inst));
        if (index) *index = i;
    });
}

UNX_API int32_t UNX_CALL UnxSceneAddLight(UnxRenderer r, const UnxLightDesc* d, uint32_t* index)
{
    return call([&] {
        requireStruct(d, "UnxLightDesc");
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
        const scene::Scene& s = h->scene();  // wind is never written after commit
        const float3 wind = f3(d->windDirection);
        if (wind.x != s.windDirection.x || wind.y != s.windDirection.y || wind.z != s.windDirection.z || d->windSpeed != s.windSpeed)
            fail("the scene wind cannot change after commit yet: the deformation bounds (INTERFACES 6.4) assume a constant wind "
                 "(request Docs/Design/Requests/20260925_I_wind_change.md); pass the committed wind");
        const scene::Sun sun = sunOf(d);
        const float len = length(sun.direction);
        if (std::abs(len - 1.0f) > 1e-3f) fail("sun direction is not unit length (%f)", len);
        h->setEnvironment(sun, atmosphereOf(d));
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
        if (!d3d) return;
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
