#include "unx/scene/SceneData.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/core/Sha256.h"

#include <cstring>
#include <fstream>
#include <type_traits>

namespace unx::scene
{
namespace
{
constexpr char kMagic[8] = { 'U', 'N', 'X', 'S', 'C', 'E', 'N', 'E' };

struct Writer
{
    std::vector<uint8_t> out;
    void bytes(const void* p, size_t n)
    {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    template <typename T>
    void pod(const T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof v);
    }
    void str(const std::string& s)
    {
        pod<uint64_t>(s.size());
        bytes(s.data(), s.size());
    }
    template <typename T>
    void podArray(const std::vector<T>& v)
    {
        pod<uint64_t>(v.size());
        if (!v.empty()) bytes(v.data(), v.size() * sizeof(T));
    }
};

struct Reader
{
    const std::vector<uint8_t>& in;
    size_t at = 0;
    void bytes(void* p, size_t n)
    {
        if (at + n > in.size()) fail("unxscene: truncated file at byte %zu", at);
        std::memcpy(p, in.data() + at, n);
        at += n;
    }
    template <typename T>
    T pod()
    {
        T v;
        bytes(&v, sizeof v);
        return v;
    }
    uint64_t count(size_t elementSize)
    {
        uint64_t n = pod<uint64_t>();
        if (elementSize && n > (in.size() - at) / elementSize) fail("unxscene: array length %llu exceeds the file", (unsigned long long)n);
        return n;
    }
    std::string str()
    {
        std::string s(count(1), '\0');
        bytes(s.data(), s.size());
        return s;
    }
    template <typename T>
    void podArray(std::vector<T>& v)
    {
        v.resize(count(sizeof(T)));
        if (!v.empty()) bytes(v.data(), v.size() * sizeof(T));
    }
};

// Explicit field order (never memcpy whole structs that hold std::string or vectors).
void write(Writer& w, const Texture& t)
{
    w.str(t.name); w.pod(t.width); w.pod(t.height); w.pod(t.format); w.pod(t.wrap); w.podArray(t.texels);
}
void read(Reader& r, Texture& t)
{
    t.name = r.str(); t.width = r.pod<uint32_t>(); t.height = r.pod<uint32_t>(); t.format = r.pod<TextureFormat>(); t.wrap = r.pod<bool>(); r.podArray(t.texels);
}
void write(Writer& w, const Material& m)
{
    w.str(m.name); w.pod(m.cls); w.pod(m.baseColor); w.pod(m.roughness); w.pod(m.metallic); w.pod(m.specular); w.pod(m.emissive);
    w.pod(m.alphaCutoff); w.pod(m.transmission); w.pod(m.ior); w.pod(m.twoSided);
    w.pod(m.baseColorTexture); w.pod(m.normalTexture); w.pod(m.roughMetalTexture); w.pod(m.emissiveTexture); w.pod(m.occlusionTexture);
}
void read(Reader& r, Material& m)
{
    m.name = r.str(); m.cls = r.pod<MaterialClass>(); m.baseColor = r.pod<float3>(); m.roughness = r.pod<float>(); m.metallic = r.pod<float>();
    m.specular = r.pod<float>(); m.emissive = r.pod<float3>(); m.alphaCutoff = r.pod<float>(); m.transmission = r.pod<float>(); m.ior = r.pod<float>();
    m.twoSided = r.pod<bool>(); m.baseColorTexture = r.pod<uint32_t>(); m.normalTexture = r.pod<uint32_t>(); m.roughMetalTexture = r.pod<uint32_t>();
    m.emissiveTexture = r.pod<uint32_t>(); m.occlusionTexture = r.pod<uint32_t>();
}
void write(Writer& w, const Mesh& m)
{
    w.str(m.name); w.podArray(m.positions); w.podArray(m.normals); w.podArray(m.tangents); w.podArray(m.uv0); w.podArray(m.indices);
    w.podArray(m.submeshes); w.podArray(m.skin.joints); w.podArray(m.skin.weights); w.podArray(m.skin.inverseBind);
}
void read(Reader& r, Mesh& m)
{
    m.name = r.str(); r.podArray(m.positions); r.podArray(m.normals); r.podArray(m.tangents); r.podArray(m.uv0); r.podArray(m.indices);
    r.podArray(m.submeshes); r.podArray(m.skin.joints); r.podArray(m.skin.weights); r.podArray(m.skin.inverseBind);
}
void write(Writer& w, const Instance& i)
{
    w.pod(i.mesh); w.pod(i.transform); w.pod(i.flags); w.pod(i.skeleton); w.pod(i.wind); w.podArray(i.materialOverrides);
}
void read(Reader& r, Instance& i)
{
    i.mesh = r.pod<uint32_t>(); i.transform = r.pod<float3x4>(); i.flags = r.pod<uint32_t>(); i.skeleton = r.pod<uint32_t>(); i.wind = r.pod<WindParams>();
    r.podArray(i.materialOverrides);
}
void write(Writer& w, const Skeleton& s) { w.str(s.name); w.podArray(s.jointToModel); }
void read(Reader& r, Skeleton& s) { s.name = r.str(); r.podArray(s.jointToModel); }
void write(Writer& w, const Light& l)
{
    w.pod(l.type); w.pod(l.position); w.pod(l.forward); w.pod(l.right); w.pod(l.color); w.pod(l.intensity); w.pod(l.range);
    w.pod(l.spotInner); w.pod(l.spotOuter); w.pod(l.size); w.pod(l.castShadow);
}
void read(Reader& r, Light& l)
{
    l.type = r.pod<LightType>(); l.position = r.pod<float3>(); l.forward = r.pod<float3>(); l.right = r.pod<float3>(); l.color = r.pod<float3>();
    l.intensity = r.pod<float>(); l.range = r.pod<float>(); l.spotInner = r.pod<float>(); l.spotOuter = r.pod<float>(); l.size = r.pod<float2>();
    l.castShadow = r.pod<bool>();
}
void write(Writer& w, const Camera& c)
{
    w.str(c.name); w.pod(c.position); w.pod(c.forward); w.pod(c.up); w.pod(c.verticalFov); w.pod(c.nearPlane); w.pod(c.ev100);
}
void read(Reader& r, Camera& c)
{
    c.name = r.str(); c.position = r.pod<float3>(); c.forward = r.pod<float3>(); c.up = r.pod<float3>(); c.verticalFov = r.pod<float>();
    c.nearPlane = r.pod<float>(); c.ev100 = r.pod<float>();
}
void write(Writer& w, const CameraPath& p) { w.str(p.name); w.podArray(p.keys); }
void read(Reader& r, CameraPath& p) { p.name = r.str(); r.podArray(p.keys); }

template <typename T>
void writeList(Writer& w, const std::vector<T>& v)
{
    w.pod<uint64_t>(v.size());
    for (const T& x : v) write(w, x);
}
template <typename T>
void readList(Reader& r, std::vector<T>& v)
{
    v.resize(r.count(0));
    for (T& x : v) read(r, x);
}

bool unit(float3 v) { return std::fabs(length(v) - 1.0f) < 1e-3f; }
} // namespace

std::vector<uint8_t> serialize(const Scene& s)
{
    Writer w;
    w.bytes(kMagic, sizeof kMagic);
    w.pod(kSceneFormatVersion);
    w.str(s.name);
    w.pod(s.seed);
    writeList(w, s.textures);
    writeList(w, s.materials);
    writeList(w, s.meshes);
    writeList(w, s.instances);
    writeList(w, s.skeletons);
    writeList(w, s.lights);
    w.pod(s.sun);
    w.pod(s.atmosphere);
    w.pod(s.windDirection);
    w.pod(s.windSpeed);
    writeList(w, s.cameras);
    writeList(w, s.paths);
    return std::move(w.out);
}

Scene deserialize(const std::vector<uint8_t>& bytes)
{
    Reader r{ bytes };
    char magic[8];
    r.bytes(magic, sizeof magic);
    if (std::memcmp(magic, kMagic, sizeof magic) != 0) fail("unxscene: bad magic");
    uint32_t version = r.pod<uint32_t>();
    if (version != kSceneFormatVersion) fail("unxscene: version %u, this build reads %u", version, kSceneFormatVersion);
    Scene s;
    s.name = r.str();
    s.seed = r.pod<uint64_t>();
    readList(r, s.textures);
    readList(r, s.materials);
    readList(r, s.meshes);
    readList(r, s.instances);
    readList(r, s.skeletons);
    readList(r, s.lights);
    s.sun = r.pod<Sun>();
    s.atmosphere = r.pod<Atmosphere>();
    s.windDirection = r.pod<float3>();
    s.windSpeed = r.pod<float>();
    readList(r, s.cameras);
    readList(r, s.paths);
    if (r.at != bytes.size()) fail("unxscene: %zu trailing bytes", bytes.size() - r.at);
    return s;
}

void save(const Scene& scene, const std::filesystem::path& path)
{
    std::vector<uint8_t> bytes = serialize(scene);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) fail("cannot write %s", path.string().c_str());
    f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
}

Scene load(const std::filesystem::path& path) { return deserialize(readBinaryFile(path)); }

std::string contentHash(const Scene& scene)
{
    std::vector<uint8_t> bytes = serialize(scene);
    return Sha256::hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void validate(const Scene& s)
{
    for (size_t i = 0; i < s.textures.size(); ++i)
    {
        const Texture& t = s.textures[i];
        const size_t texelBytes = t.format == TextureFormat::Rgba16Float ? 8 : t.format == TextureFormat::R8Linear ? 1
                                  : (t.format == TextureFormat::Rg8Normal || t.format == TextureFormat::Rg8RoughMetal) ? 2 : 4;
        if ((size_t)t.width * t.height * texelBytes != t.texels.size()) fail("texture %zu '%s': %zu bytes for %ux%u", i, t.name.c_str(), t.texels.size(), t.width, t.height);
    }
    auto texOk = [&](uint32_t t) { return t == kNone || t < s.textures.size(); };
    for (size_t i = 0; i < s.materials.size(); ++i)
    {
        const Material& m = s.materials[i];
        if (!texOk(m.baseColorTexture) || !texOk(m.normalTexture) || !texOk(m.roughMetalTexture) || !texOk(m.emissiveTexture) || !texOk(m.occlusionTexture))
            fail("material %zu '%s': texture index out of range", i, m.name.c_str());
        if (m.roughness < 0 || m.roughness > 1 || m.metallic < 0 || m.metallic > 1) fail("material %zu '%s': roughness/metallic outside [0,1]", i, m.name.c_str());
    }
    for (size_t i = 0; i < s.meshes.size(); ++i)
    {
        const Mesh& m = s.meshes[i];
        const size_t n = m.positions.size();
        if (m.normals.size() != n || (!m.uv0.empty() && m.uv0.size() != n) || (!m.tangents.empty() && m.tangents.size() != n))
            fail("mesh %zu '%s': vertex stream sizes differ", i, m.name.c_str());
        if (m.indices.size() % 3) fail("mesh %zu '%s': index count not a multiple of 3", i, m.name.c_str());
        for (uint32_t idx : m.indices)
            if (idx >= n) fail("mesh %zu '%s': index %u out of range", i, m.name.c_str(), idx);
        for (const Submesh& sm : m.submeshes)
            if (sm.indexOffset + sm.indexCount > m.indices.size() || sm.indexCount % 3 || sm.material >= s.materials.size())
                fail("mesh %zu '%s': bad submesh", i, m.name.c_str());
        if (!m.skin.joints.empty() && (m.skin.joints.size() != 4 * n || m.skin.weights.size() != 4 * n)) fail("mesh %zu '%s': skin stream size", i, m.name.c_str());
        for (const Submesh& sm : m.submeshes)
            if (s.materials[sm.material].normalTexture != kNone && (m.tangents.empty() || m.uv0.empty()))
                fail("mesh %zu '%s': a normal-mapped material needs tangents and uv0", i, m.name.c_str());
    }
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        const Instance& in = s.instances[i];
        if (in.mesh >= s.meshes.size()) fail("instance %zu: mesh out of range", i);
        if ((in.flags & InstanceSkinned) && (in.skeleton >= s.skeletons.size() || s.meshes[in.mesh].skin.joints.empty())) fail("instance %zu: skinned without skeleton/skin", i);
        if (!in.materialOverrides.empty() && in.materialOverrides.size() != s.meshes[in.mesh].submeshes.size()) fail("instance %zu: material overrides per submesh", i);
        // Rotation + uniform scale: the three basis columns have equal length and are orthogonal.
        const float3 cx{ in.transform.m[0][0], in.transform.m[1][0], in.transform.m[2][0] };
        const float3 cy{ in.transform.m[0][1], in.transform.m[1][1], in.transform.m[2][1] };
        const float3 cz{ in.transform.m[0][2], in.transform.m[1][2], in.transform.m[2][2] };
        const float sx = length(cx), sy = length(cy), sz = length(cz);
        if (std::fabs(sx - sy) > 1e-3f * sx || std::fabs(sx - sz) > 1e-3f * sx || std::fabs(dot(cx, cy)) > 1e-3f * sx * sy || std::fabs(dot(cx, cz)) > 1e-3f * sx * sz)
            fail("instance %zu: transform must be rotation + uniform scale + translation", i);
    }
    for (size_t i = 0; i < s.lights.size(); ++i)
        if (!unit(s.lights[i].forward)) fail("light %zu: forward not unit", i);
    if (!unit(s.sun.direction)) fail("sun direction not unit");
    for (const Camera& c : s.cameras)
        if (!unit(c.forward) || !unit(c.up)) fail("camera '%s': forward/up not unit", c.name.c_str());
}
} // namespace unx::scene
