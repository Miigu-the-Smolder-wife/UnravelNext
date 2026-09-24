#include "RtScene.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::reference
{
struct RtScene::MeshData
{
    const RtScene* owner = nullptr;
    const scene::Mesh* src = nullptr;
    uint32_t meshIndex = 0;
    uint32_t deformedInstance = ~0u;           // scene instance when this is a deformed world-space copy
    std::vector<float3> positions, normals;    // deformed copies (world space); empty -> src streams
    std::vector<float4> tangents;
    std::vector<uint16_t> triSubmesh;
    bool alpha = false;
    RTCScene scene = nullptr;                  // shared meshes: the instanced scene
    const float3* pos() const { return positions.empty() ? src->positions.data() : positions.data(); }
    const float3* nrm() const { return normals.empty() ? src->normals.data() : normals.data(); }
    const float4* tan() const { return tangents.empty() ? (src->tangents.empty() ? nullptr : src->tangents.data()) : tangents.data(); }
};

struct RtScene::Group
{
    uint32_t mesh = 0;
    std::vector<uint32_t> instances;  // instPrimID -> scene instance
};

namespace
{
float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float halfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

float3x4 mul34(const float3x4& a, const float3x4& b)
{
    float3x4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float s = j == 3 ? a.m[i][3] : 0.0f;
            for (int k = 0; k < 3; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

float3 xformVector(const float3x4& m, float3 v) { return m.transformVector(v); }

void alphaFilter(const RTCFilterFunctionNArguments* args);

// Wind v1 (the model of Passes/Common/Deformation.hlsli, with the world->object direction as the transpose; see
// Docs/Design/Requests/20260925_C_wind_direction.md): object-space displacement.
float3 windOffset(const scene::Scene& s, const scene::Instance& in, float3 p, float time)
{
    if (in.wind.stiffness <= 0 || s.windSpeed <= 0) return {};
    const float h = std::max(p.y - in.wind.anchorHeight, 0.0f);
    const float amplitude = s.windSpeed * s.windSpeed * 0.002f / in.wind.stiffness * h * h;
    const float3 d = s.windDirection;
    const float3x4& m = in.transform;
    const float3 dirObject = normalize(float3{ m.m[0][0] * d.x + m.m[1][0] * d.y + m.m[2][0] * d.z, m.m[0][1] * d.x + m.m[1][1] * d.y + m.m[2][1] * d.z,
                                               m.m[0][2] * d.x + m.m[1][2] * d.y + m.m[2][2] * d.z });
    return dirObject * (amplitude * (0.6f + 0.4f * std::sin(time * 1.7f + in.wind.phase)));
}

bool windActive(const scene::Scene& s, const scene::Instance& in)
{
    return (in.flags & scene::InstanceWind) && in.wind.stiffness > 0 && s.windSpeed > 0;
}
} // namespace

Texture::Texture(const scene::Texture& t) : m_w(t.width), m_h(t.height), m_wrap(t.wrap)
{
    m_texels.resize((size_t)m_w * m_h);
    float lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = srgbToLinear(i / 255.0f);
    for (size_t i = 0; i < m_texels.size(); ++i)
    {
        Texel& x = m_texels[i];
        switch (t.format)
        {
        case scene::TextureFormat::Rgba8Srgb:
            x = { lut[t.texels[4 * i]], lut[t.texels[4 * i + 1]], lut[t.texels[4 * i + 2]], t.texels[4 * i + 3] / 255.0f };
            break;
        case scene::TextureFormat::Rgba8Linear:
            x = { t.texels[4 * i] / 255.0f, t.texels[4 * i + 1] / 255.0f, t.texels[4 * i + 2] / 255.0f, t.texels[4 * i + 3] / 255.0f };
            break;
        case scene::TextureFormat::Rg8Normal:
        case scene::TextureFormat::Rg8RoughMetal:
            x = { t.texels[2 * i] / 255.0f, t.texels[2 * i + 1] / 255.0f, 0, 1 };
            break;
        case scene::TextureFormat::R8Linear:
            x = { t.texels[i] / 255.0f, 0, 0, 1 };
            break;
        case scene::TextureFormat::Rgba16Float:
        {
            uint16_t h[4];
            std::memcpy(h, &t.texels[8 * i], 8);
            x = { halfToFloat(h[0]), halfToFloat(h[1]), halfToFloat(h[2]), halfToFloat(h[3]) };
            break;
        }
        }
    }
}

Texel Texture::sample(float2 uv) const
{
    // Texel centres at (i + 0.5) / size; bilinear between the four nearest.
    const float x = uv.x * m_w - 0.5f, y = uv.y * m_h - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1, y1 = y0 + 1;
    auto fix = [&](int& i, uint32_t n) {
        if (m_wrap) i = ((i % (int)n) + (int)n) % (int)n;
        else i = std::clamp(i, 0, (int)n - 1);
    };
    fix(x0, m_w);
    fix(x1, m_w);
    fix(y0, m_h);
    fix(y1, m_h);
    const Texel& a = m_texels[(size_t)y0 * m_w + x0];
    const Texel& b = m_texels[(size_t)y0 * m_w + x1];
    const Texel& c = m_texels[(size_t)y1 * m_w + x0];
    const Texel& d = m_texels[(size_t)y1 * m_w + x1];
    auto lerp2 = [&](float pa, float pb, float pc, float pd) { return (pa * (1 - tx) + pb * tx) * (1 - ty) + (pc * (1 - tx) + pd * tx) * ty; };
    return { lerp2(a.r, b.r, c.r, d.r), lerp2(a.g, b.g, c.g, d.g), lerp2(a.b, b.b, c.b, d.b), lerp2(a.a, b.a, c.a, d.a) };
}

RtScene::RtScene(const scene::Scene& s, float time, uint32_t threads) : m_scene(s)
{
    scene::validate(s);
    for (const scene::Material& m : s.materials)
    {
        if (m.cls != scene::MaterialClass::Standard && m.cls != scene::MaterialClass::Foliage)
            fail("reference: material '%s' uses a class without a v1 model (INTERFACES 8.1 defines Standard and Foliage)", m.name.c_str());
        if (m.occlusionTexture != scene::kNone)
            fail("reference: material '%s' has an occlusion texture; its use is not defined in INTERFACES 8.1 v1", m.name.c_str());
    }
    m_textures.reserve(s.textures.size());
    for (const scene::Texture& t : s.textures) m_textures.emplace_back(t);

    const std::string config = threads ? format("threads=%u,set_affinity=0", threads) : std::string("set_affinity=0");
    m_device = rtcNewDevice(config.c_str());
    if (!m_device) fail("reference: rtcNewDevice failed (%d)", (int)rtcGetDeviceError(nullptr));
    if (!rtcGetDeviceProperty(m_device, RTC_DEVICE_PROPERTY_RAY_MASK_SUPPORTED)) fail("reference: this Embree build has no ray masks");
    m_top = rtcNewScene(m_device);
    rtcSetSceneBuildQuality(m_top, RTC_BUILD_QUALITY_HIGH);

    auto materialAlpha = [&](uint32_t mat) { return s.materials[mat].alphaCutoff > 0 && s.materials[mat].baseColorTexture != scene::kNone; };
    auto buildTriSubmesh = [](MeshData& md) {
        md.triSubmesh.assign(md.src->indices.size() / 3, 0);
        for (size_t k = 0; k < md.src->submeshes.size(); ++k)
        {
            const scene::Submesh& sm = md.src->submeshes[k];
            for (uint32_t t = sm.indexOffset / 3; t < (sm.indexOffset + sm.indexCount) / 3; ++t) md.triSubmesh[t] = (uint16_t)k;
        }
    };
    auto makeGeometry = [&](MeshData& md, bool alpha) {
        RTCGeometry g = rtcNewGeometry(m_device, RTC_GEOMETRY_TYPE_TRIANGLE);
        const size_t nv = md.src->positions.size(), nt = md.src->indices.size() / 3;
        float* v = (float*)rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), nv);
        std::memcpy(v, md.pos(), nv * sizeof(float3));
        uint32_t* idx = (uint32_t*)rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(uint32_t), nt);
        std::memcpy(idx, md.src->indices.data(), nt * 3 * sizeof(uint32_t));
        rtcSetGeometryUserData(g, &md);
        // Embree 4 tests masks at every instancing level and its default geometry mask is 0x1: the instanced
        // geometry must accept every ray; the instance (array) mask decides shadow casting.
        rtcSetGeometryMask(g, 0xFFFFFFFFu);
        if (alpha)
        {
            rtcSetGeometryIntersectFilterFunction(g, alphaFilter);
            rtcSetGeometryOccludedFilterFunction(g, alphaFilter);
        }
        rtcCommitGeometry(g);
        return g;
    };

    // Which instances need their own deformed geometry, and which shared meshes need alpha filtering.
    m_instanceDeformed.assign(s.instances.size(), -1);
    std::vector<bool> meshAlpha(s.meshes.size(), false), meshUsedShared(s.meshes.size(), false);
    uint64_t deformedTris = 0;
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        const scene::Instance& in = s.instances[i];
        const scene::Mesh& m = s.meshes[in.mesh];
        bool alpha = false;
        for (size_t k = 0; k < m.submeshes.size(); ++k)
            alpha |= materialAlpha(in.materialOverrides.empty() ? m.submeshes[k].material : in.materialOverrides[k]);
        const bool skinned = (in.flags & scene::InstanceSkinned) != 0, wind = windActive(s, in);
        if (skinned || wind)
        {
            deformedTris += m.indices.size() / 3;
            m_deform.skinnedInstances += skinned;
            m_deform.windInstances += wind;
            m_instanceDeformed[i] = 0;  // marked; filled below
        }
        else
        {
            meshAlpha[in.mesh] = meshAlpha[in.mesh] || alpha;
            meshUsedShared[in.mesh] = true;
        }
    }
    m_deform.deformedTriangles = deformedTris;
    if (deformedTris > kMaxDeformedTriangles)
        fail("reference: %u wind and %u skinned instances need %.1f M deformed triangles (limit %.0f M). Exact wind for this scene "
             "needs per-instance deformation inside the traversal (not implemented yet); render it with --no-wind",
             m_deform.windInstances, m_deform.skinnedInstances, deformedTris / 1e6, kMaxDeformedTriangles / 1e6);

    // Shared meshes.
    m_meshes.resize(s.meshes.size());
    for (size_t mi = 0; mi < s.meshes.size(); ++mi)
    {
        auto md = std::make_unique<MeshData>();
        md->owner = this;
        md->src = &s.meshes[mi];
        md->meshIndex = (uint32_t)mi;
        md->alpha = meshAlpha[mi];
        buildTriSubmesh(*md);
        if (meshUsedShared[mi])
        {
            md->scene = rtcNewScene(m_device);
            rtcSetSceneBuildQuality(md->scene, RTC_BUILD_QUALITY_HIGH);
            RTCGeometry g = makeGeometry(*md, md->alpha);
            rtcAttachGeometry(md->scene, g);
            rtcReleaseGeometry(g);
            rtcCommitScene(md->scene);
        }
        m_meshes[mi] = std::move(md);
    }

    // Instance arrays grouped by (mesh, casts shadow), in first-use order.
    std::vector<int32_t> groupOf(s.meshes.size() * 2, -1);
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        if (m_instanceDeformed[i] >= 0) continue;
        const scene::Instance& in = s.instances[i];
        const size_t key = (size_t)in.mesh * 2 + ((in.flags & scene::InstanceCastShadow) ? 1 : 0);
        if (groupOf[key] < 0)
        {
            groupOf[key] = (int32_t)m_groups.size();
            auto g = std::make_unique<Group>();
            g->mesh = in.mesh;
            m_groups.push_back(std::move(g));
        }
        m_groups[groupOf[key]]->instances.push_back((uint32_t)i);
    }
    for (size_t key = 0; key < groupOf.size(); ++key)
    {
        if (groupOf[key] < 0) continue;
        Group& grp = *m_groups[groupOf[key]];
        RTCGeometry g = rtcNewGeometry(m_device, RTC_GEOMETRY_TYPE_INSTANCE_ARRAY);
        rtcSetGeometryInstancedScene(g, m_meshes[grp.mesh]->scene);
        float* xf = (float*)rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_TRANSFORM, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, 12 * sizeof(float), grp.instances.size());
        for (size_t k = 0; k < grp.instances.size(); ++k) std::memcpy(xf + 12 * k, &s.instances[grp.instances[k]].transform.m[0][0], 12 * sizeof(float));
        rtcSetGeometryMask(g, kMaskAll | ((key & 1) ? kMaskShadow : 0));
        rtcCommitGeometry(g);
        const uint32_t id = rtcAttachGeometry(m_top, g);
        rtcReleaseGeometry(g);
        if (m_geomToGroup.size() <= id) m_geomToGroup.resize(id + 1, -1);
        m_geomToGroup[id] = groupOf[key];
    }

    // Deformed instances: world-space copies.
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        if (m_instanceDeformed[i] < 0) continue;
        const scene::Instance& in = s.instances[i];
        const scene::Mesh& m = s.meshes[in.mesh];
        auto md = std::make_unique<MeshData>();
        md->owner = this;
        md->src = &m;
        md->meshIndex = in.mesh;
        md->deformedInstance = (uint32_t)i;
        buildTriSubmesh(*md);
        const size_t n = m.positions.size();
        md->positions.resize(n);
        md->normals.resize(n);
        if (!m.tangents.empty()) md->tangents.resize(n);
        std::vector<float3x4> palette;
        if (in.flags & scene::InstanceSkinned)
        {
            const scene::Skeleton& sk = s.skeletons[in.skeleton];
            if (sk.jointToModel.size() < m.skin.inverseBind.size()) fail("reference: skeleton '%s' has fewer joints than mesh '%s'", sk.name.c_str(), m.name.c_str());
            palette.resize(m.skin.inverseBind.size());
            for (size_t j = 0; j < palette.size(); ++j) palette[j] = mul34(sk.jointToModel[j], m.skin.inverseBind[j]);
        }
        for (size_t v = 0; v < n; ++v)
        {
            float3 p = m.positions[v], nn = m.normals[v];
            float3 t = m.tangents.empty() ? float3{} : float3{ m.tangents[v].x, m.tangents[v].y, m.tangents[v].z };
            if (!palette.empty())
            {
                float3 sp{}, sn{}, st{};
                for (int k = 0; k < 4; ++k)
                {
                    const float w = m.skin.weights[4 * v + k];
                    const float3x4& jm = palette[m.skin.joints[4 * v + k]];
                    sp = sp + jm.transformPoint(p) * w;
                    sn = sn + jm.transformVector(nn) * w;
                    st = st + jm.transformVector(t) * w;
                }
                p = sp;
                nn = normalize(sn);
                if (!m.tangents.empty()) t = normalize(st);
            }
            if (windActive(s, in)) p = p + windOffset(s, in, p, time);
            md->positions[v] = in.transform.transformPoint(p);
            md->normals[v] = normalize(xformVector(in.transform, nn));
            if (!m.tangents.empty())
            {
                const float3 tw = normalize(xformVector(in.transform, t));
                md->tangents[v] = { tw.x, tw.y, tw.z, m.tangents[v].w };
            }
        }
        bool alpha = false;
        for (size_t k = 0; k < m.submeshes.size(); ++k)
            alpha |= materialAlpha(in.materialOverrides.empty() ? m.submeshes[k].material : in.materialOverrides[k]);
        md->alpha = alpha;
        RTCGeometry g = makeGeometry(*md, alpha);
        rtcSetGeometryMask(g, kMaskAll | ((in.flags & scene::InstanceCastShadow) ? kMaskShadow : 0));
        rtcCommitGeometry(g);
        const uint32_t id = rtcAttachGeometry(m_top, g);
        rtcReleaseGeometry(g);
        if (m_geomToDeformed.size() <= id) m_geomToDeformed.resize(id + 1, -1);
        m_instanceDeformed[i] = (int32_t)m_deformed.size();
        m_geomToDeformed[id] = (int32_t)m_deformed.size();
        m_deformed.push_back(std::move(md));
    }
    m_geomToGroup.resize(std::max(m_geomToGroup.size(), m_geomToDeformed.size()), -1);
    m_geomToDeformed.resize(m_geomToGroup.size(), -1);
    rtcCommitScene(m_top);
    const RTCError err = rtcGetDeviceError(m_device);
    if (err != RTC_ERROR_NONE) fail("reference: Embree error %d while building the scene", (int)err);
}

RtScene::~RtScene()
{
    if (m_top) rtcReleaseScene(m_top);
    for (auto& m : m_meshes)
        if (m && m->scene) rtcReleaseScene(m->scene);
    if (m_device) rtcReleaseDevice(m_device);
}

const RtScene::MeshData& RtScene::meshOf(uint32_t instance) const
{
    const int32_t d = m_instanceDeformed[instance];
    return d >= 0 ? *m_deformed[d] : *m_meshes[m_scene.instances[instance].mesh];
}

uint32_t RtScene::materialOf(uint32_t instance, const MeshData& md, uint32_t triangle) const
{
    const uint32_t sub = md.triSubmesh[triangle];
    const scene::Instance& in = m_scene.instances[instance];
    return in.materialOverrides.empty() ? md.src->submeshes[sub].material : in.materialOverrides[sub];
}

bool RtScene::alphaOpaque(uint32_t instance, uint32_t triangle, float u, float v) const
{
    const MeshData& md = meshOf(instance);
    const scene::Material& mat = m_scene.materials[materialOf(instance, md, triangle)];
    if (mat.alphaCutoff <= 0 || mat.baseColorTexture == scene::kNone) return true;
    const uint32_t* tri = &md.src->indices[3 * (size_t)triangle];
    const float2 a = md.src->uv0[tri[0]], b = md.src->uv0[tri[1]], c = md.src->uv0[tri[2]];
    const float w = 1 - u - v;
    const float2 uv{ a.x * w + b.x * u + c.x * v, a.y * w + b.y * u + c.y * v };
    return m_textures[mat.baseColorTexture].sample(uv).a >= mat.alphaCutoff;
}

namespace
{
void alphaFilter(const RTCFilterFunctionNArguments* args)
{
    const auto* md = (const RtScene::MeshData*)args->geometryUserPtr;
    for (unsigned i = 0; i < args->N; ++i)
    {
        if (args->valid[i] != -1) continue;
        RTCHitN* h = args->hit;
        const uint32_t prim = RTCHitN_primID(h, args->N, i);
        const float u = RTCHitN_u(h, args->N, i), v = RTCHitN_v(h, args->N, i);
        uint32_t instance = md->deformedInstance;
        if (instance == ~0u)
        {
            const uint32_t geom = RTCHitN_instID(h, args->N, i, 0), sub = RTCHitN_instPrimID(h, args->N, i, 0);
            instance = md->owner->instanceOf(geom, sub);
        }
        if (!md->owner->alphaOpaque(instance, prim, u, v)) args->valid[i] = 0;
    }
}
} // namespace

uint32_t RtScene::instanceOf(uint32_t topGeom, uint32_t instPrim) const { return m_groups[m_geomToGroup[topGeom]]->instances[instPrim]; }

bool RtScene::intersect(float3 o, float3 d, float tnear, float tfar, uint32_t mask, Hit& hit) const
{
    RTCRayHit rh{};
    rh.ray.org_x = o.x;
    rh.ray.org_y = o.y;
    rh.ray.org_z = o.z;
    rh.ray.dir_x = d.x;
    rh.ray.dir_y = d.y;
    rh.ray.dir_z = d.z;
    rh.ray.tnear = tnear;
    rh.ray.tfar = tfar;
    rh.ray.mask = mask;
    rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    RTCIntersectArguments args;
    rtcInitIntersectArguments(&args);
    rtcIntersect1(m_top, &rh, &args);
    if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID) return false;
    if (rh.hit.instID[0] != RTC_INVALID_GEOMETRY_ID) hit.instance = instanceOf(rh.hit.instID[0], rh.hit.instPrimID[0]);
    else hit.instance = m_deformed[m_geomToDeformed[rh.hit.geomID]]->deformedInstance;
    hit.triangle = rh.hit.primID;
    hit.u = rh.hit.u;
    hit.v = rh.hit.v;
    hit.t = rh.ray.tfar;
    return true;
}

bool RtScene::occluded(float3 o, float3 d, float tnear, float tfar) const
{
    RTCRay r{};
    r.org_x = o.x;
    r.org_y = o.y;
    r.org_z = o.z;
    r.dir_x = d.x;
    r.dir_y = d.y;
    r.dir_z = d.z;
    r.tnear = tnear;
    r.tfar = tfar;
    r.mask = kMaskShadow;
    RTCOccludedArguments args;
    rtcInitOccludedArguments(&args);
    rtcOccluded1(m_top, &r, &args);
    return r.tfar < 0;
}

Surface RtScene::surface(const Hit& hit, float3 rayDir) const
{
    const MeshData& md = meshOf(hit.instance);
    const scene::Instance& in = m_scene.instances[hit.instance];
    const bool world = md.deformedInstance != ~0u;
    const uint32_t* tri = &md.src->indices[3 * (size_t)hit.triangle];
    const float w = 1 - hit.u - hit.v, u = hit.u, v = hit.v;
    const float3* P = md.pos();
    const float3* N = md.nrm();
    const float4* T = md.tan();
    auto toWorldP = [&](float3 p) { return world ? p : in.transform.transformPoint(p); };
    auto toWorldV = [&](float3 x) { return world ? x : in.transform.transformVector(x); };
    const float3 p0 = toWorldP(P[tri[0]]), p1 = toWorldP(P[tri[1]]), p2 = toWorldP(P[tri[2]]);
    Surface s;
    s.p = p0 * w + p1 * u + p2 * v;
    s.ng = normalize(cross(p1 - p0, p2 - p0));
    float3 n = normalize(toWorldV(N[tri[0]] * w + N[tri[1]] * u + N[tri[2]] * v));
    s.material = materialOf(hit.instance, md, hit.triangle);
    const scene::Material& mat = m_scene.materials[s.material];
    float2 uv{};
    if (!md.src->uv0.empty())
    {
        const float2 a = md.src->uv0[tri[0]], b = md.src->uv0[tri[1]], c = md.src->uv0[tri[2]];
        uv = { a.x * w + b.x * u + c.x * v, a.y * w + b.y * u + c.y * v };
    }
    float3 base = mat.baseColor;
    if (mat.baseColorTexture != scene::kNone)
    {
        const Texel t = m_textures[mat.baseColorTexture].sample(uv);
        base = base * float3{ t.r, t.g, t.b };
    }
    float rough = mat.roughness, metal = mat.metallic;
    if (mat.roughMetalTexture != scene::kNone)
    {
        const Texel t = m_textures[mat.roughMetalTexture].sample(uv);
        rough *= t.r;
        metal *= t.g;
    }
    if (mat.normalTexture != scene::kNone && T)
    {
        const float4 t0 = T[tri[0]], t1 = T[tri[1]], t2 = T[tri[2]];
        const float3 tl{ t0.x * w + t1.x * u + t2.x * v, t0.y * w + t1.y * u + t2.y * v, t0.z * w + t1.z * u + t2.z * v };
        float3 tg = toWorldV(tl);
        tg = tg - n * dot(n, tg);
        if (dot(tg, tg) > 1e-20f)
        {
            tg = normalize(tg);
            const float3 bt = cross(n, tg) * t0.w;
            const Texel nt = m_textures[mat.normalTexture].sample(uv);
            const float x = 2 * nt.r - 1, y = 2 * nt.g - 1, z = std::sqrt(std::max(0.0f, 1 - x * x - y * y));
            n = normalize(tg * x + bt * y + n * z);
        }
    }
    // Keep the shading normal on the geometric side of the surface it belongs to.
    if (dot(n, s.ng) < 0) n = n - s.ng * (2 * dot(n, s.ng));
    const float3 wo = -rayDir;
    s.frontFacing = dot(s.ng, wo) >= 0;
    if (!s.frontFacing && mat.twoSided)
    {
        s.ng = -s.ng;
        n = -n;
        s.frontFacing = true;
    }
    // Interpolated/mapped normals can face away from the viewer on a front-facing surface; bend them just enough
    // to keep n.v > 0 (the BRDF is defined for n.v > 0 only).
    const float nv = dot(n, wo);
    if (s.frontFacing && nv < 1e-4f) n = normalize(n + wo * (1e-4f - nv));
    s.ns = n;
    s.bsdf.cls = mat.cls;
    s.bsdf.baseColor = base;
    s.bsdf.roughness = std::clamp(rough, 0.0f, 1.0f);
    s.bsdf.metallic = std::clamp(metal, 0.0f, 1.0f);
    s.bsdf.specular = mat.specular;
    s.bsdf.transmission = mat.transmission;
    if (mat.emissive.x > 0 || mat.emissive.y > 0 || mat.emissive.z > 0)
    {
        float3 e = mat.emissive;
        if (mat.emissiveTexture != scene::kNone)
        {
            const Texel t = m_textures[mat.emissiveTexture].sample(uv);
            e = e * float3{ t.r, t.g, t.b };
        }
        if (s.frontFacing) s.emission = Rgb(e);
    }
    return s;
}

float3 offsetRayOrigin(float3 p, float3 n)
{
    constexpr float origin = 1.0f / 32.0f, floatScale = 1.0f / 65536.0f, intScale = 256.0f;
    const float pc[3] = { p.x, p.y, p.z }, nc[3] = { n.x, n.y, n.z };
    float out[3];
    for (int k = 0; k < 3; ++k)
    {
        const int32_t oi = (int32_t)(intScale * nc[k]);
        int32_t bits;
        std::memcpy(&bits, &pc[k], 4);
        bits += pc[k] < 0 ? -oi : oi;
        float pi;
        std::memcpy(&pi, &bits, 4);
        out[k] = std::fabs(pc[k]) < origin ? pc[k] + floatScale * nc[k] : pi;
    }
    return { out[0], out[1], out[2] };
}
} // namespace unx::reference
