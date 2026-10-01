#include "unx/scene/SceneData.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/core/Sha256.h"

#include <algorithm>
#include <cmath>
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

// C4 extension block, written after every other field only when some mesh or instance uses blend shapes or a vertex
// animation, so scenes without them keep their bytes and content hashes: u32 tag "MRPH", then per mesh (blend shapes,
// vertex animation) and per instance (weights, time).
constexpr uint32_t kMorphTag = 0x4850524Du;  // "MRPH"

// B10 hair extension block (INTERFACES 8.1 v1.66), written only when the scene has a Hair-class material (other scenes
// keep their bytes and content hashes): u32 tag "HAIR", u64 count, then per hair material its index and eumelanin,
// pheomelanin, beta_N, tilt.
constexpr uint32_t kHairTag = 0x52494148u;  // "HAIR"

bool anyHair(const Scene& s)
{
    for (const Material& m : s.materials)
        if (m.cls == MaterialClass::Hair) return true;
    return false;
}

void writeHair(Writer& w, const Scene& s)
{
    w.pod(kHairTag);
    uint64_t count = 0;
    for (const Material& m : s.materials) count += m.cls == MaterialClass::Hair;
    w.pod(count);
    for (uint32_t i = 0; i < s.materials.size(); ++i)
    {
        const Material& m = s.materials[i];
        if (m.cls != MaterialClass::Hair) continue;
        w.pod(i);
        w.pod(m.hairEumelanin);
        w.pod(m.hairPheomelanin);
        w.pod(m.hairBetaN);
        w.pod(m.hairTilt);
    }
}

// A11 cut extension block, written only when the scene has a Cut-class material: u32 tag "CUTS", u64 count, then per
// cut material its index, cutScale, cutDamageWidth.
constexpr uint32_t kCutTag = 0x53545543u;  // "CUTS"

bool anyCut(const Scene& s)
{
    for (const Material& m : s.materials)
        if (m.cls == MaterialClass::Cut) return true;
    return false;
}

void writeCut(Writer& w, const Scene& s)
{
    w.pod(kCutTag);
    uint64_t count = 0;
    for (const Material& m : s.materials) count += m.cls == MaterialClass::Cut;
    w.pod(count);
    for (uint32_t i = 0; i < s.materials.size(); ++i)
    {
        const Material& m = s.materials[i];
        if (m.cls != MaterialClass::Cut) continue;
        w.pod(i);
        w.pod(m.cutScale);
        w.pod(m.cutDamageWidth);
    }
}

// C5 terrain extension block, written only when the scene has a Terrain-class material: u32 tag "TERR", u64 count, then
// per terrain material its index, the two splat textures, the layer count and per layer (material, scale xy, offset xy).
constexpr uint32_t kTerrainTag = 0x52524554u;  // "TERR"

bool anyTerrain(const Scene& s)
{
    for (const Material& m : s.materials)
        if (m.cls == MaterialClass::Terrain) return true;
    return false;
}

void writeTerrain(Writer& w, const Scene& s)
{
    w.pod(kTerrainTag);
    uint64_t count = 0;
    for (const Material& m : s.materials) count += m.cls == MaterialClass::Terrain;
    w.pod(count);
    for (uint32_t i = 0; i < s.materials.size(); ++i)
    {
        const Material& m = s.materials[i];
        if (m.cls != MaterialClass::Terrain) continue;
        w.pod(i);
        w.pod(m.terrainSplat[0]);
        w.pod(m.terrainSplat[1]);
        const uint32_t layers = (uint32_t)m.terrainLayers.size();
        w.pod(layers);
        for (const TerrainLayer& l : m.terrainLayers)
        {
            w.pod(l.material);
            w.pod(l.scale);
            w.pod(l.offset);
        }
    }
}

// A9 clearcoat extension block, written only when a material has a coat: u32 tag "COAT", u64 count, then per coated
// material its index, clearcoat, clearcoatRoughness, clearcoatIor.
constexpr uint32_t kCoatTag = 0x54414F43u;  // "COAT"

bool anyCoat(const Scene& s)
{
    for (const Material& m : s.materials)
        if (m.clearcoat > 0) return true;
    return false;
}

void writeCoat(Writer& w, const Scene& s)
{
    w.pod(kCoatTag);
    uint64_t count = 0;
    for (const Material& m : s.materials) count += m.clearcoat > 0;
    w.pod(count);
    for (uint32_t i = 0; i < s.materials.size(); ++i)
    {
        const Material& m = s.materials[i];
        if (!(m.clearcoat > 0)) continue;
        w.pod(i);
        w.pod(m.clearcoat);
        w.pod(m.clearcoatRoughness);
        w.pod(m.clearcoatIor);
    }
}

// A9 sheen extension block, written only when a material has a sheen: u32 tag "SHEN", u64 count, then per material its
// index, sheenColor, sheenRoughness.
constexpr uint32_t kSheenTag = 0x4E454853u;  // "SHEN"

bool hasSheen(const Material& m) { return m.sheenColor.x > 0 || m.sheenColor.y > 0 || m.sheenColor.z > 0; }

bool anySheen(const Scene& s)
{
    for (const Material& m : s.materials)
        if (hasSheen(m)) return true;
    return false;
}

void writeSheen(Writer& w, const Scene& s)
{
    w.pod(kSheenTag);
    uint64_t count = 0;
    for (const Material& m : s.materials) count += hasSheen(m);
    w.pod(count);
    for (uint32_t i = 0; i < s.materials.size(); ++i)
    {
        const Material& m = s.materials[i];
        if (!hasSheen(m)) continue;
        w.pod(i);
        w.pod(m.sheenColor);
        w.pod(m.sheenRoughness);
    }
}

// A9 anisotropy extension block, written only when a material is anisotropic: u32 tag "ANIS", u64 count, then per
// material its index, anisotropy, anisotropyRotation.
constexpr uint32_t kAnisoTag = 0x53494E41u;  // "ANIS"

bool hasAnisotropy(const Material& m) { return m.anisotropy != 0.0f || m.anisotropyRotation != 0.0f; }

bool anyAnisotropy(const Scene& s)
{
    for (const Material& m : s.materials)
        if (hasAnisotropy(m)) return true;
    return false;
}

void writeAnisotropy(Writer& w, const Scene& s)
{
    w.pod(kAnisoTag);
    uint64_t count = 0;
    for (const Material& m : s.materials) count += hasAnisotropy(m);
    w.pod(count);
    for (uint32_t i = 0; i < s.materials.size(); ++i)
    {
        const Material& m = s.materials[i];
        if (!hasAnisotropy(m)) continue;
        w.pod(i);
        w.pod(m.anisotropy);
        w.pod(m.anisotropyRotation);
    }
}

// A9 thin film extension block, written only when a material has a film: u32 tag "FILM", u64 count, then per material
// its index, thinFilmThickness, thinFilmIor, thinFilmCoverage, thinFilmSubstrate, substrateIor, substrateExtinction.
constexpr uint32_t kFilmTag = 0x4D4C4946u;  // "FILM"

bool hasFilm(const Material& m) { return m.thinFilmThickness != 0.0f; }

bool anyFilm(const Scene& s)
{
    for (const Material& m : s.materials)
        if (hasFilm(m)) return true;
    return false;
}

void writeFilm(Writer& w, const Scene& s)
{
    w.pod(kFilmTag);
    uint64_t count = 0;
    for (const Material& m : s.materials) count += hasFilm(m);
    w.pod(count);
    for (uint32_t i = 0; i < s.materials.size(); ++i)
    {
        const Material& m = s.materials[i];
        if (!hasFilm(m)) continue;
        w.pod(i);
        w.pod(m.thinFilmThickness);
        w.pod(m.thinFilmIor);
        w.pod(m.thinFilmCoverage);
        w.pod(m.thinFilmSubstrate);
        w.pod(m.substrateIor);
        w.pod(m.substrateExtinction);
    }
}

// A10 glass extension block, written only when a Glass material's attenuation distance is not the default: u32 tag
// "GATT", u64 count, then per material its index and attenuationDistance.
constexpr uint32_t kGlassTag = 0x54544147u;  // "GATT"

bool hasAttenuation(const Material& m) { return m.cls == MaterialClass::Glass && m.attenuationDistance != 0.01f; }

bool anyAttenuation(const Scene& s)
{
    for (const Material& m : s.materials)
        if (hasAttenuation(m)) return true;
    return false;
}

void writeAttenuation(Writer& w, const Scene& s)
{
    w.pod(kGlassTag);
    uint64_t count = 0;
    for (const Material& m : s.materials) count += hasAttenuation(m);
    w.pod(count);
    for (uint32_t i = 0; i < s.materials.size(); ++i)
        if (hasAttenuation(s.materials[i]))
        {
            w.pod(i);
            w.pod(s.materials[i].attenuationDistance);
        }
}

bool anyMorph(const Scene& s)
{
    for (const Mesh& m : s.meshes)
        if (!m.blendShapes.empty() || m.vertexAnimation.framesPerSecond > 0) return true;
    for (const Instance& i : s.instances)
        if (!i.blendWeights.empty() || i.vertexAnimationTime != 0) return true;
    return false;
}

void writeMorph(Writer& w, const Scene& s)
{
    w.pod(kMorphTag);
    for (const Mesh& m : s.meshes)
    {
        w.pod<uint64_t>(m.blendShapes.size());
        for (const BlendShape& b : m.blendShapes)
        {
            w.str(b.name);
            w.podArray(b.vertices);
            w.podArray(b.deltaPositions);
            w.podArray(b.deltaNormals);
        }
        const VertexAnimation& a = m.vertexAnimation;
        w.pod(a.framesPerSecond);
        w.pod(a.frameCount);
        w.pod(a.loop);
        w.podArray(a.positions);
        w.podArray(a.normals);
    }
    for (const Instance& i : s.instances)
    {
        w.podArray(i.blendWeights);
        w.pod(i.vertexAnimationTime);
    }
}

// Lights with their own shadow-ray end bias (Light::rayEndBias >= 0; INTERFACES v1.93): an optional block after the
// material blocks, as those are - a scene without such lights writes the same bytes as before.
constexpr uint32_t kLightEndTag = 0x444E454Cu;  // "LEND"

bool anyLightEnd(const Scene& s)
{
    for (const Light& l : s.lights)
        if (l.rayEndBias >= 0) return true;
    return false;
}

void writeLightEnd(Writer& w, const Scene& s)
{
    w.pod(kLightEndTag);
    uint64_t count = 0;
    for (const Light& l : s.lights) count += l.rayEndBias >= 0;
    w.pod(count);
    for (uint32_t i = 0; i < s.lights.size(); ++i)
        if (s.lights[i].rayEndBias >= 0)
        {
            w.pod(i);
            w.pod(s.lights[i].rayEndBias);
        }
}

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
    if (anyMorph(s)) writeMorph(w, s);
    if (anyHair(s)) writeHair(w, s);
    if (anyCut(s)) writeCut(w, s);
    if (anyTerrain(s)) writeTerrain(w, s);
    if (anyCoat(s)) writeCoat(w, s);
    if (anySheen(s)) writeSheen(w, s);
    if (anyAttenuation(s)) writeAttenuation(w, s);
    if (anyAnisotropy(s)) writeAnisotropy(w, s);
    if (anyFilm(s)) writeFilm(w, s);
    if (anyLightEnd(s)) writeLightEnd(w, s);
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
    uint32_t tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    if (tag == kMorphTag)
    {
        for (Mesh& m : s.meshes)
        {
            m.blendShapes.resize((size_t)r.pod<uint64_t>());
            for (BlendShape& b : m.blendShapes)
            {
                b.name = r.str();
                r.podArray(b.vertices);
                r.podArray(b.deltaPositions);
                r.podArray(b.deltaNormals);
            }
            VertexAnimation& a = m.vertexAnimation;
            a.framesPerSecond = r.pod<float>();
            a.frameCount = r.pod<uint32_t>();
            a.loop = r.pod<bool>();
            r.podArray(a.positions);
            r.podArray(a.normals);
        }
        for (Instance& i : s.instances)
        {
            r.podArray(i.blendWeights);
            i.vertexAnimationTime = r.pod<float>();
        }
        tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    }
    if (tag == kHairTag)
    {
        const uint64_t count = r.pod<uint64_t>();
        for (uint64_t k = 0; k < count; ++k)
        {
            const uint32_t i = r.pod<uint32_t>();
            if (i >= s.materials.size()) fail("unxscene: hair parameters of material %u of %zu", i, s.materials.size());
            Material& m = s.materials[i];
            m.hairEumelanin = r.pod<float>();
            m.hairPheomelanin = r.pod<float>();
            m.hairBetaN = r.pod<float>();
            m.hairTilt = r.pod<float>();
        }
        tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    }
    if (tag == kCutTag)
    {
        const uint64_t count = r.pod<uint64_t>();
        for (uint64_t k = 0; k < count; ++k)
        {
            const uint32_t i = r.pod<uint32_t>();
            if (i >= s.materials.size()) fail("unxscene: cut parameters of material %u of %zu", i, s.materials.size());
            s.materials[i].cutScale = r.pod<float>();
            s.materials[i].cutDamageWidth = r.pod<float>();
        }
        tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    }
    if (tag == kTerrainTag)
    {
        const uint64_t count = r.pod<uint64_t>();
        for (uint64_t k = 0; k < count; ++k)
        {
            const uint32_t i = r.pod<uint32_t>();
            if (i >= s.materials.size()) fail("unxscene: terrain parameters of material %u of %zu", i, s.materials.size());
            Material& m = s.materials[i];
            m.terrainSplat[0] = r.pod<uint32_t>();
            m.terrainSplat[1] = r.pod<uint32_t>();
            const uint32_t layers = r.pod<uint32_t>();
            if (layers > 8) fail("unxscene: terrain material %u has %u layers (at most 8)", i, layers);
            m.terrainLayers.resize(layers);
            for (TerrainLayer& l : m.terrainLayers)
            {
                l.material = r.pod<uint32_t>();
                l.scale = r.pod<float2>();
                l.offset = r.pod<float2>();
            }
        }
        tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    }
    if (tag == kCoatTag)
    {
        const uint64_t count = r.pod<uint64_t>();
        for (uint64_t k = 0; k < count; ++k)
        {
            const uint32_t i = r.pod<uint32_t>();
            if (i >= s.materials.size()) fail("unxscene: clearcoat parameters of material %u of %zu", i, s.materials.size());
            s.materials[i].clearcoat = r.pod<float>();
            s.materials[i].clearcoatRoughness = r.pod<float>();
            s.materials[i].clearcoatIor = r.pod<float>();
        }
        tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    }
    if (tag == kSheenTag)
    {
        const uint64_t count = r.pod<uint64_t>();
        for (uint64_t k = 0; k < count; ++k)
        {
            const uint32_t i = r.pod<uint32_t>();
            if (i >= s.materials.size()) fail("unxscene: sheen parameters of material %u of %zu", i, s.materials.size());
            s.materials[i].sheenColor = r.pod<float3>();
            s.materials[i].sheenRoughness = r.pod<float>();
        }
        tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    }
    if (tag == kGlassTag)
    {
        const uint64_t count = r.pod<uint64_t>();
        for (uint64_t k = 0; k < count; ++k)
        {
            const uint32_t i = r.pod<uint32_t>();
            if (i >= s.materials.size()) fail("unxscene: glass attenuation of material %u of %zu", i, s.materials.size());
            s.materials[i].attenuationDistance = r.pod<float>();
        }
        tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    }
    if (tag == kAnisoTag)
    {
        const uint64_t count = r.pod<uint64_t>();
        for (uint64_t k = 0; k < count; ++k)
        {
            const uint32_t i = r.pod<uint32_t>();
            if (i >= s.materials.size()) fail("unxscene: anisotropy of material %u of %zu", i, s.materials.size());
            s.materials[i].anisotropy = r.pod<float>();
            s.materials[i].anisotropyRotation = r.pod<float>();
        }
        tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    }
    if (tag == kFilmTag)
    {
        const uint64_t count = r.pod<uint64_t>();
        for (uint64_t k = 0; k < count; ++k)
        {
            const uint32_t i = r.pod<uint32_t>();
            if (i >= s.materials.size()) fail("unxscene: thin film of material %u of %zu", i, s.materials.size());
            Material& m = s.materials[i];
            m.thinFilmThickness = r.pod<float>();
            m.thinFilmIor = r.pod<float>();
            m.thinFilmCoverage = r.pod<float>();
            m.thinFilmSubstrate = r.pod<uint32_t>();
            m.substrateIor = r.pod<float>();
            m.substrateExtinction = r.pod<float>();
        }
        tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    }
    if (tag == kLightEndTag)
    {
        const uint64_t count = r.pod<uint64_t>();
        for (uint64_t k = 0; k < count; ++k)
        {
            const uint32_t i = r.pod<uint32_t>();
            if (i >= s.lights.size()) fail("unxscene: ray end bias of light %u of %zu", i, s.lights.size());
            s.lights[i].rayEndBias = r.pod<float>();
        }
        tag = r.at < bytes.size() ? r.pod<uint32_t>() : 0;
    }
    if (tag != 0) fail("unxscene: unknown extension block 0x%08x", tag);
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
        if (m.cls == MaterialClass::Hair && !(m.hairEumelanin >= 0 && m.hairPheomelanin >= 0 && m.hairBetaN > 0 && m.hairBetaN <= 1 && std::isfinite(m.hairTilt) && m.ior > 1))
            fail("material %zu '%s': hair needs melanin >= 0, beta_N in (0, 1], a finite tilt and ior > 1", i, m.name.c_str());
        if (!(m.waterScattering.x >= 0 && m.waterScattering.y >= 0 && m.waterScattering.z >= 0 && std::isfinite(m.waterScattering.x) && std::isfinite(m.waterScattering.y) &&
              std::isfinite(m.waterScattering.z) && m.waterAnisotropy > -1 && m.waterAnisotropy < 1))
            fail("material %zu '%s': water scattering >= 0 (finite) and anisotropy in (-1, 1)", i, m.name.c_str());
        if (m.cls == MaterialClass::Cut && !(m.cutScale > 0 && m.cutDamageWidth >= 0 && std::isfinite(m.cutScale) && std::isfinite(m.cutDamageWidth)))
            fail("material %zu '%s': a cut material needs cutScale > 0 and cutDamageWidth >= 0", i, m.name.c_str());
        if (m.clearcoat != 0)
        {
            if (!(m.clearcoat > 0 && m.clearcoat <= 1 && m.clearcoatRoughness >= 0 && m.clearcoatRoughness <= 1))
                fail("material %zu '%s': clearcoat in (0, 1] and clearcoatRoughness in [0, 1]", i, m.name.c_str());
            if (m.cls != MaterialClass::Standard)
                fail("material %zu '%s': a clearcoat is defined on Standard materials", i, m.name.c_str());
            if (m.clearcoatIor != 1.5f && m.clearcoatIor != 1.33f)
                fail("material %zu '%s': clearcoatIor %g is not a tabulated coat (1.5 or 1.33)", i, m.name.c_str(), m.clearcoatIor);
        }
        if (m.cls == MaterialClass::Glass && !(m.attenuationDistance > 0))
            fail("material %zu '%s': attenuationDistance > 0 (m)", i, m.name.c_str());
        if (hasSheen(m))
        {
            if (!(m.sheenColor.x >= 0 && m.sheenColor.x <= 1 && m.sheenColor.y >= 0 && m.sheenColor.y <= 1 && m.sheenColor.z >= 0 && m.sheenColor.z <= 1))
                fail("material %zu '%s': sheenColor in [0, 1]", i, m.name.c_str());
            if (!(m.sheenRoughness >= 0.1f && m.sheenRoughness <= 1))
                fail("material %zu '%s': sheenRoughness in [0.1, 1] (the lobe stays much wider than the sun's disk)", i, m.name.c_str());
            if (m.cls != MaterialClass::Standard) fail("material %zu '%s': a sheen is defined on Standard materials", i, m.name.c_str());
            if (m.clearcoat > 0) fail("material %zu '%s': one layer kind per material (sheen or clearcoat)", i, m.name.c_str());
        }
        if (hasAnisotropy(m))
        {
            if (!(m.anisotropy >= 0 && m.anisotropy <= 1)) fail("material %zu '%s': anisotropy in [0, 1]", i, m.name.c_str());
            if (!std::isfinite(m.anisotropyRotation)) fail("material %zu '%s': anisotropyRotation finite (radians)", i, m.name.c_str());
            if (m.cls != MaterialClass::Standard) fail("material %zu '%s': anisotropy is defined on Standard materials", i, m.name.c_str());
        }
        if (hasFilm(m))
        {
            if (!(m.thinFilmThickness > 0 && m.thinFilmThickness <= 5000)) fail("material %zu '%s': thinFilmThickness in (0, 5000] nm", i, m.name.c_str());
            if (!(m.thinFilmIor >= 1 && m.thinFilmIor <= 3)) fail("material %zu '%s': thinFilmIor in [1, 3]", i, m.name.c_str());
            if (!(m.thinFilmCoverage >= 0 && m.thinFilmCoverage <= 1)) fail("material %zu '%s': thinFilmCoverage in [0, 1]", i, m.name.c_str());
            if (m.thinFilmSubstrate > 5) fail("material %zu '%s': thinFilmSubstrate 0..5 (constant, gold, copper, silver, aluminium, iron)", i, m.name.c_str());
            if (!(m.substrateIor >= 1 && m.substrateIor <= 5 && m.substrateExtinction >= 0 && m.substrateExtinction <= 20))
                fail("material %zu '%s': substrateIor in [1, 5], substrateExtinction in [0, 20]", i, m.name.c_str());
            if (m.cls != MaterialClass::Standard) fail("material %zu '%s': a thin film is defined on Standard materials", i, m.name.c_str());
            // The film under a coat (outer index 1.5) and with the sheen or anisotropic lobes is not joined yet (FEATURE_STATUS A9).
            if (m.clearcoat > 0 || hasSheen(m) || hasAnisotropy(m))
                fail("material %zu '%s': a thin film is not combined with a clearcoat, sheen or anisotropy", i, m.name.c_str());
        }
        if (m.cls == MaterialClass::Terrain)
        {
            const size_t layers = m.terrainLayers.size();
            if (layers < 1 || layers > 8) fail("material %zu '%s': a terrain material needs 1..8 layers (has %zu)", i, m.name.c_str(), layers);
            for (uint32_t k = 0; k < (layers > 4 ? 2u : 1u); ++k)
                if (m.terrainSplat[k] >= s.textures.size() || s.textures[m.terrainSplat[k]].format != TextureFormat::Rgba8Linear)
                    fail("material %zu '%s': terrain splat map %u must be an Rgba8Linear texture", i, m.name.c_str(), k);
            for (const TerrainLayer& l : m.terrainLayers)
            {
                if (l.material >= s.materials.size() || s.materials[l.material].cls != MaterialClass::Standard)
                    fail("material %zu '%s': terrain layer material %u must be a Standard-class material", i, m.name.c_str(), l.material);
                if (!(std::isfinite(l.scale.x) && std::isfinite(l.scale.y) && l.scale.x != 0 && l.scale.y != 0 && std::isfinite(l.offset.x) && std::isfinite(l.offset.y)))
                    fail("material %zu '%s': terrain layer uv scale must be finite and non-zero, offset finite", i, m.name.c_str());
            }
        }
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
        for (const Submesh& sm : m.submeshes)
            if (s.materials[sm.material].anisotropy > 0 && m.tangents.empty())
                fail("mesh %zu '%s': an anisotropic material needs tangents (the lobe's direction)", i, m.name.c_str());
        for (const BlendShape& b : m.blendShapes)
        {
            if (b.deltaPositions.size() != b.vertices.size() || (!b.deltaNormals.empty() && b.deltaNormals.size() != b.vertices.size()))
                fail("mesh %zu '%s': blend shape '%s' stream sizes differ", i, m.name.c_str(), b.name.c_str());
            for (size_t k = 0; k < b.vertices.size(); ++k)
                if (b.vertices[k] >= n || (k > 0 && b.vertices[k] <= b.vertices[k - 1]))
                    fail("mesh %zu '%s': blend shape '%s' vertices must be ascending mesh vertices", i, m.name.c_str(), b.name.c_str());
        }
        const VertexAnimation& va = m.vertexAnimation;
        if (va.framesPerSecond < 0 || !std::isfinite(va.framesPerSecond)) fail("mesh %zu '%s': vertex animation rate", i, m.name.c_str());
        if (va.framesPerSecond > 0)
        {
            if (va.frameCount == 0 || va.positions.size() != (size_t)va.frameCount * n || (!va.normals.empty() && va.normals.size() != va.positions.size()))
                fail("mesh %zu '%s': vertex animation stream sizes", i, m.name.c_str());
            if (!m.skin.joints.empty() || !m.blendShapes.empty()) fail("mesh %zu '%s': a vertex-animated mesh has no skin and no blend shapes", i, m.name.c_str());
        }
    }
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        const Instance& in = s.instances[i];
        if (in.mesh >= s.meshes.size()) fail("instance %zu: mesh out of range", i);
        if ((in.flags & InstanceSkinned) && (in.skeleton >= s.skeletons.size() || s.meshes[in.mesh].skin.joints.empty())) fail("instance %zu: skinned without skeleton/skin", i);
        if (!in.materialOverrides.empty() && in.materialOverrides.size() != s.meshes[in.mesh].submeshes.size()) fail("instance %zu: material overrides per submesh", i);
        if (!in.blendWeights.empty() && in.blendWeights.size() != s.meshes[in.mesh].blendShapes.size()) fail("instance %zu: blend weights per blend shape", i);
        for (float wgt : in.blendWeights)
            if (!std::isfinite(wgt)) fail("instance %zu: blend weight not finite", i);
        if (!std::isfinite(in.vertexAnimationTime)) fail("instance %zu: vertex animation time not finite", i);
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
void evaluateMorph(const Mesh& m, const std::vector<float>& weights, float time, uint32_t v, float3& position, float3& normal)
{
    const VertexAnimation& a = m.vertexAnimation;
    const size_t n = m.positions.size();
    if (a.framesPerSecond > 0)
    {
        const double f = (double)time * a.framesPerSecond;
        double base = std::floor(f);
        const float t = (float)(f - base);
        uint64_t f0, f1;
        if (a.loop)
        {
            const double c = a.frameCount;
            base = base - c * std::floor(base / c);
            f0 = (uint64_t)base;
            f1 = (f0 + 1) % a.frameCount;
        }
        else
        {
            f0 = (uint64_t)std::clamp(base, 0.0, (double)a.frameCount - 1);
            f1 = std::min<uint64_t>(f0 + 1, a.frameCount - 1);
            if (base < 0 || base >= a.frameCount - 1) f1 = f0;
        }
        const float3 p0 = a.positions[f0 * n + v], p1 = a.positions[f1 * n + v];
        position = p0 + (p1 - p0) * t;
        const float3 n0 = a.normals.empty() ? m.normals[v] : a.normals[f0 * n + v], n1 = a.normals.empty() ? m.normals[v] : a.normals[f1 * n + v];
        normal = normalize(n0 + (n1 - n0) * t);
        return;
    }
    position = m.positions[v];
    float3 nrm = m.normals[v];
    for (size_t s = 0; s < m.blendShapes.size() && s < weights.size(); ++s)
    {
        const BlendShape& b = m.blendShapes[s];
        if (weights[s] == 0) continue;
        const auto it = std::lower_bound(b.vertices.begin(), b.vertices.end(), v);
        if (it == b.vertices.end() || *it != v) continue;
        const size_t k = (size_t)(it - b.vertices.begin());
        position = position + b.deltaPositions[k] * weights[s];
        if (!b.deltaNormals.empty()) nrm = nrm + b.deltaNormals[k] * weights[s];
    }
    normal = normalize(nrm);
}

float morphBound(const Mesh& m, const std::vector<float>& weightBounds)
{
    const VertexAnimation& a = m.vertexAnimation;
    const size_t n = m.positions.size();
    float bound = 0;
    if (a.framesPerSecond > 0)
    {
        for (size_t f = 0; f < a.frameCount; ++f)
            for (size_t v = 0; v < n; ++v) bound = std::max(bound, length(a.positions[f * n + v] - m.positions[v]));
        return bound;
    }
    // Per vertex, sum over the shapes that move it of |w_s| max |dp|: bounded by the sum over shapes of the shape's
    // largest offset, which is what a per-instance culling inflation can carry.
    for (size_t s = 0; s < m.blendShapes.size() && s < weightBounds.size(); ++s)
    {
        float largest = 0;
        for (const float3& d : m.blendShapes[s].deltaPositions) largest = std::max(largest, length(d));
        bound += std::fabs(weightBounds[s]) * largest;
    }
    return bound;
}
} // namespace unx::scene
