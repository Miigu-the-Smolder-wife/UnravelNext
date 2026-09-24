#include "Common.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cstring>

namespace unx::scenegen::detail
{
Rng::Rng(uint64_t seed, uint64_t stream)
{
    inc = (stream << 1u) | 1u;
    state = 0;
    next();
    state += seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    next();
}

uint32_t Rng::next()
{
    const uint64_t old = state;
    state = old * 6364136223846793005ull + inc;
    const uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    const uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
}

float3x4 placement(float3 position, float yaw, float scale, float tilt)
{
    const float cy = std::cos(yaw), sy = std::sin(yaw), ct = std::cos(tilt), st = std::sin(tilt);
    // R = Ry(yaw) * Rx(tilt)
    const float r[3][3] = { { cy, sy * st, sy * ct }, { 0, ct, -st }, { -sy, cy * st, cy * ct } };
    float3x4 m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m.m[i][j] = r[i][j] * scale;
    m.m[0][3] = position.x;
    m.m[1][3] = position.y;
    m.m[2][3] = position.z;
    return m;
}

float3x4 frame(float3 position, float3 up, float3 right, float scale)
{
    const float3 y = normalize(up);
    const float3 x = normalize(right - y * dot(right, y));
    const float3 z = cross(x, y);
    float3x4 m;
    const float3 cols[3] = { x, y, z };
    for (int j = 0; j < 3; ++j)
    {
        m.m[0][j] = cols[j].x * scale;
        m.m[1][j] = cols[j].y * scale;
        m.m[2][j] = cols[j].z * scale;
    }
    m.m[0][3] = position.x;
    m.m[1][3] = position.y;
    m.m[2][3] = position.z;
    return m;
}

namespace
{
uint32_t hash3(int32_t x, int32_t y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}
int wrapi(int i, int period) { return period > 0 ? ((i % period) + period) % period : i; }
} // namespace

float valueNoise2(float x, float y, uint32_t seed, int period)
{
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = (int)fx, iy = (int)fy;
    const float tx = x - fx, ty = y - fy;
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    auto v = [&](int a, int b) { return (hash3(wrapi(a, period), wrapi(b, period), seed) >> 8) * (1.0f / 16777216.0f); };
    const float a = v(ix, iy), b = v(ix + 1, iy), c = v(ix, iy + 1), d = v(ix + 1, iy + 1);
    return lerpf(lerpf(a, b, sx), lerpf(c, d, sx), sy);
}

float fbm2(float x, float y, uint32_t seed, int octaves, int period)
{
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < octaves; ++o)
    {
        sum += amp * valueNoise2(x, y, seed + (uint32_t)o * 101u, period);
        norm += amp;
        x *= 2;
        y *= 2;
        period *= 2;
        amp *= 0.5f;
    }
    return sum / norm;
}

uint32_t MeshBuilder::vertex(float3 p, float3 n, float2 uv)
{
    mesh.positions.push_back(p);
    mesh.normals.push_back(normalize(n));
    mesh.uv0.push_back(uv);
    return (uint32_t)mesh.positions.size() - 1;
}

void MeshBuilder::material(uint32_t m)
{
    const uint32_t end = (uint32_t)mesh.indices.size();
    if (end > submeshStart) mesh.submeshes.push_back({ submeshStart, end - submeshStart, currentMaterial });
    submeshStart = end;
    currentMaterial = m;
}

Mesh MeshBuilder::finish(bool withTangents)
{
    material(currentMaterial);
    if (withTangents) computeTangents(mesh);
    return std::move(mesh);
}

void MeshBuilder::quad4(float3 p0, float3 p1, float3 p2, float3 p3, float2 t0, float2 t1, float2 t2, float2 t3)
{
    const float3 n = normalize(cross(p1 - p0, p2 - p0));
    const uint32_t a = vertex(p0, n, t0), b = vertex(p1, n, t1), c = vertex(p2, n, t2), d = vertex(p3, n, t3);
    quad(a, b, c, d);
}

void MeshBuilder::box(float3 lo, float3 hi, float uvScale)
{
    const float s = uvScale;
    const float x0 = lo.x, y0 = lo.y, z0 = lo.z, x1 = hi.x, y1 = hi.y, z1 = hi.z;
    // UVs: u along the face's horizontal axis, v downwards (texture rows top to bottom), in metres * uvScale.
    quad4({ x1, y0, z1 }, { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { z1 * s, -y0 * s }, { z0 * s, -y0 * s }, { z0 * s, -y1 * s }, { z1 * s, -y1 * s });       // +X
    quad4({ x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 }, { -z0 * s, -y0 * s }, { -z1 * s, -y0 * s }, { -z1 * s, -y1 * s }, { -z0 * s, -y1 * s }); // -X
    quad4({ x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 }, { x0 * s, -y0 * s }, { x1 * s, -y0 * s }, { x1 * s, -y1 * s }, { x0 * s, -y1 * s });       // +Z
    quad4({ x1, y0, z0 }, { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { -x1 * s, -y0 * s }, { -x0 * s, -y0 * s }, { -x0 * s, -y1 * s }, { -x1 * s, -y1 * s }); // -Z
    quad4({ x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 }, { x0, y1, z0 }, { x0 * s, z1 * s }, { x1 * s, z1 * s }, { x1 * s, z0 * s }, { x0 * s, z0 * s });       // +Y
    quad4({ x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 }, { x0 * s, -z0 * s }, { x1 * s, -z0 * s }, { x1 * s, -z1 * s }, { x0 * s, -z1 * s });   // -Y
}

void MeshBuilder::cylinder(float3 base, float3 axis, float r0, float r1, int segments, int rings, bool caps, float uvScale)
{
    const float h = length(axis);
    const float3 a = axis / h;
    const float3 ref = std::fabs(a.y) < 0.9f ? f3(0, 1, 0) : f3(1, 0, 0);
    const float3 u = normalize(cross(ref, a));
    const float3 w = cross(a, u);
    const uint32_t first = (uint32_t)mesh.positions.size();
    const float circumference = 2 * kPi * std::max(r0, r1);
    for (int ri = 0; ri <= rings; ++ri)
    {
        const float t = (float)ri / rings;
        const float r = lerpf(r0, r1, t);
        for (int si = 0; si <= segments; ++si)
        {
            const float ang = 2 * kPi * si / segments;
            const float3 radial = u * std::cos(ang) + w * std::sin(ang);
            const float3 n = radial * h + a * (r0 - r1);
            vertex(base + a * (t * h) + radial * r, n, { (float)si / segments * circumference * uvScale, -t * h * uvScale });
        }
    }
    const uint32_t stride = (uint32_t)segments + 1;
    for (int ri = 0; ri < rings; ++ri)
        for (int si = 0; si < segments; ++si)
        {
            const uint32_t p00 = first + ri * stride + si, p10 = p00 + 1, p01 = p00 + stride, p11 = p01 + 1;
            quad(p00, p10, p11, p01);
        }
    if (caps)
    {
        for (int end = 0; end < 2; ++end)
        {
            const float r = end ? r1 : r0;
            if (r <= 0) continue;
            const float3 c = end ? base + axis : base;
            const float3 n = end ? a : -a;
            const uint32_t centre = vertex(c, n, { 0, 0 });
            const uint32_t ring = (uint32_t)mesh.positions.size();
            for (int si = 0; si < segments; ++si)
            {
                const float ang = 2 * kPi * si / segments;
                const float3 radial = u * std::cos(ang) + w * std::sin(ang);
                vertex(c + radial * r, n, { std::cos(ang) * r * uvScale, std::sin(ang) * r * uvScale });
            }
            for (int si = 0; si < segments; ++si)
            {
                const uint32_t i0 = ring + si, i1 = ring + (si + 1) % segments;
                if (end) triangle(centre, i0, i1);
                else triangle(centre, i1, i0);
            }
        }
    }
}

void MeshBuilder::sphere(float3 centre, float radius, int segments, int rings, float uvScale)
{
    const uint32_t first = (uint32_t)mesh.positions.size();
    for (int ri = 0; ri <= rings; ++ri)
    {
        const float th = kPi * ri / rings;  // from +Y down
        for (int si = 0; si <= segments; ++si)
        {
            const float ph = 2 * kPi * si / segments;
            const float3 n{ std::sin(th) * std::cos(ph), std::cos(th), -std::sin(th) * std::sin(ph) };
            vertex(centre + n * radius, n, { (float)si / segments * 2 * kPi * radius * uvScale, th * radius * uvScale });
        }
    }
    const uint32_t stride = (uint32_t)segments + 1;
    for (int ri = 0; ri < rings; ++ri)
        for (int si = 0; si < segments; ++si)
        {
            const uint32_t p00 = first + ri * stride + si, p10 = p00 + 1, p01 = p00 + stride, p11 = p01 + 1;
            // Rows go from +Y downwards and phi turns clockwise seen from above: (p00, p01, p11, p10) faces outwards.
            if (ri > 0) triangle(p00, p01, p10);
            if (ri < rings - 1) triangle(p10, p01, p11);
        }
}

void MeshBuilder::quadXZ(float x0, float z0, float x1, float z1, float y, float uvScale)
{
    const float s = uvScale;
    quad4({ x0, y, z0 }, { x0, y, z1 }, { x1, y, z1 }, { x1, y, z0 }, { x0 * s, z0 * s }, { x0 * s, z1 * s }, { x1 * s, z1 * s }, { x1 * s, z0 * s });
}

void MeshBuilder::heightfield(float x0, float z0, float x1, float z1, int n, const std::function<float(float, float)>& height, float uvScale)
{
    const uint32_t first = (uint32_t)mesh.positions.size();
    const float dx = (x1 - x0) / n, dz = (z1 - z0) / n;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
        {
            const float x = x0 + i * dx, z = z0 + j * dz;
            const float e = 0.25f * std::min(dx, dz);
            const float hx = (height(x + e, z) - height(x - e, z)) / (2 * e), hz = (height(x, z + e) - height(x, z - e)) / (2 * e);
            vertex({ x, height(x, z), z }, { -hx, 1, -hz }, { x * uvScale, z * uvScale });
        }
    const uint32_t stride = (uint32_t)n + 1;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const uint32_t a = first + j * stride + i, b = a + 1, d = a + stride, c = d + 1;
            quad(a, d, c, b);
        }
}

void computeTangents(Mesh& m)
{
    const size_t n = m.positions.size();
    std::vector<float3> tan(n), bit(n);
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
    {
        const uint32_t i0 = m.indices[t], i1 = m.indices[t + 1], i2 = m.indices[t + 2];
        const float3 e1 = m.positions[i1] - m.positions[i0], e2 = m.positions[i2] - m.positions[i0];
        const float du1 = m.uv0[i1].x - m.uv0[i0].x, dv1 = m.uv0[i1].y - m.uv0[i0].y;
        const float du2 = m.uv0[i2].x - m.uv0[i0].x, dv2 = m.uv0[i2].y - m.uv0[i0].y;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-20f) continue;
        const float r = 1.0f / det;
        const float3 T = (e1 * dv2 - e2 * dv1) * r, B = (e2 * du1 - e1 * du2) * r;
        for (uint32_t i : { i0, i1, i2 })
        {
            tan[i] = tan[i] + T;
            bit[i] = bit[i] + B;
        }
    }
    m.tangents.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        const float3 nn = m.normals[i];
        float3 t = tan[i] - nn * dot(nn, tan[i]);
        if (length(t) < 1e-12f)
        {
            const float3 ref = std::fabs(nn.y) < 0.9f ? f3(0, 1, 0) : f3(1, 0, 0);
            t = cross(ref, nn);
        }
        t = normalize(t);
        const float sign = dot(cross(nn, t), bit[i]) < 0 ? -1.0f : 1.0f;
        m.tangents[i] = { t.x, t.y, t.z, sign };
    }
}

Texture makeTexture(std::string name, uint32_t w, uint32_t h, scene::TextureFormat format, bool wrap)
{
    Texture t;
    t.name = std::move(name);
    t.width = w;
    t.height = h;
    t.format = format;
    t.wrap = wrap;
    const size_t bpp = format == scene::TextureFormat::Rgba16Float ? 8 : format == scene::TextureFormat::R8Linear ? 1
                       : (format == scene::TextureFormat::Rg8Normal || format == scene::TextureFormat::Rg8RoughMetal) ? 2 : 4;
    t.texels.resize((size_t)w * h * bpp);
    return t;
}

uint8_t toUnorm8(float v) { return (uint8_t)std::lround(clampf(v, 0, 1) * 255.0f); }

uint8_t toSrgb8(float linear)
{
    const float c = clampf(linear, 0, 1);
    const float s = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
    return toUnorm8(s);
}

Texture normalMapFromHeight(std::string name, uint32_t size, float tileMetres, const std::function<float(float, float)>& height)
{
    Texture t = makeTexture(std::move(name), size, size, scene::TextureFormat::Rg8Normal);
    const float texel = tileMetres / size;
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size, du = 1.0f / size;
            const float hu = (height(u + du, v) - height(u - du, v)) / (2 * texel);
            const float hv = (height(u, v + du) - height(u, v - du)) / (2 * texel);
            const float3 n = normalize(f3(-hu, -hv, 1));
            t.texels[2 * ((size_t)y * size + x)] = toUnorm8(0.5f * n.x + 0.5f);
            t.texels[2 * ((size_t)y * size + x) + 1] = toUnorm8(0.5f * n.y + 0.5f);
        }
    return t;
}

uint32_t addTexture(Scene& s, Texture t)
{
    s.textures.push_back(std::move(t));
    return (uint32_t)s.textures.size() - 1;
}
uint32_t addMaterial(Scene& s, Material m)
{
    s.materials.push_back(std::move(m));
    return (uint32_t)s.materials.size() - 1;
}
uint32_t addMesh(Scene& s, Mesh m)
{
    s.meshes.push_back(std::move(m));
    return (uint32_t)s.meshes.size() - 1;
}
Instance& addInstance(Scene& s, uint32_t mesh, const float3x4& transform, uint32_t flags)
{
    Instance in;
    in.mesh = mesh;
    in.transform = transform;
    in.flags = flags;
    s.instances.push_back(std::move(in));
    return s.instances.back();
}

float rollingTerrain(float x, float z)
{
    return 6.f * std::sin(x / 70.f) * std::cos(z / 90.f) + 2.f * std::sin(x / 13.f + z / 17.f) + 0.7f * std::sin(x / 3.1f - z / 2.7f);
}

float cityTerrain(float x, float z)
{
    // Flat (y = 0) inside the city square of half size 260 m, blending into gentle hills outside.
    const float d = std::max(std::fabs(x), std::fabs(z));
    const float w = smoothstepf(260.0f, 420.0f, d);
    return w * (8.f * std::sin(x / 110.f) * std::cos(z / 130.f) + 3.f * std::sin(x / 37.f + z / 53.f) + 4.f);
}

float3 sunDirection(float elevationDegrees, float azimuthDegrees)
{
    const float e = elevationDegrees * kPi / 180, a = azimuthDegrees * kPi / 180;
    return normalize(f3(std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a)));
}

float3 luminanceNormalised(float3 c) { return c / (0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z); }

scene::Camera camera(std::string name, float3 position, float3 target, float ev100, float verticalFovDegrees)
{
    scene::Camera c;
    c.name = std::move(name);
    c.position = position;
    c.forward = normalize(target - position);
    // Looking straight up or down: 'up' is -Z (image top towards -Z).
    const float3 worldUp = std::fabs(c.forward.y) > 0.999f ? f3(0, 0, -1) : f3(0, 1, 0);
    const float3 right = normalize(cross(c.forward, worldUp));
    c.up = normalize(cross(right, c.forward));
    c.ev100 = ev100;
    c.verticalFov = verticalFovDegrees * kPi / 180.0f;
    return c;
}

scene::CameraPath staticPath(const scene::Camera& c)
{
    scene::CameraPath p;
    p.name = c.name + "_static";
    for (float t : { 0.0f, 10.0f }) p.keys.push_back({ t, c.position, c.forward, c.up });
    return p;
}

scene::CameraPath linearPath(std::string name, float3 a, float3 b, float speed, float3 lookOffset)
{
    scene::CameraPath p;
    p.name = std::move(name);
    const float dist = length(b - a);
    const float3 fwd = normalize(b - a + lookOffset);
    const float3 right = normalize(cross(fwd, f3(0, 1, 0)));
    const float3 up = normalize(cross(right, fwd));
    const int steps = std::max(2, (int)std::ceil(dist / (speed * 1.0f)));  // one key per second of travel
    for (int i = 0; i <= steps; ++i)
    {
        const float t = (float)i / steps;
        p.keys.push_back({ t * dist / speed, a + (b - a) * t, fwd, up });
    }
    return p;
}

scene::CameraPath orbitPath(std::string name, float3 centre, float radius, float height, float angularSpeed, float seconds)
{
    scene::CameraPath p;
    p.name = std::move(name);
    const int steps = std::max(8, (int)std::ceil(seconds * 4));
    for (int i = 0; i <= steps; ++i)
    {
        const float t = seconds * i / steps, a = angularSpeed * t;
        const float3 pos = centre + f3(radius * std::cos(a), height, radius * std::sin(a));
        const float3 fwd = normalize(centre - pos);
        const float3 right = normalize(cross(fwd, f3(0, 1, 0)));
        p.keys.push_back({ t, pos, fwd, normalize(cross(right, fwd)) });
    }
    return p;
}
} // namespace unx::scenegen::detail
