// The reference estimator (see PathTracer.h for the model list).
//
// Path structure. Every segment o -> (surface | planet ground | top of atmosphere) crosses the atmosphere:
//   1. Emission of area lights the segment passes (after a surface vertex, MIS-weighted against light sampling) is
//      added with the transmittance up to the light; lights are not occluders, so the segment continues behind them.
//   2. Forced in-scattering NEE: a point y on the segment is drawn with pdf p(t); sun and one local light are
//      connected with the phase function. This estimates all light scattered once on this segment towards the
//      previous vertex. It runs with probability p_force = min(1, 16 (1 - T_seg)) (Russian roulette on a small term,
//      unbiased).
//   3. Continuation: with probability 1 - T_seg the path scatters at a point drawn with the same pdf (weights carry
//      the exact per-channel transmittance and scattering), otherwise it reaches the segment end with weight
//      T_seg / T_seg,avg. After a medium vertex, sun and analytic-light emission are not counted again (step 2 already
//      estimated direct light at every point of the previous segment); emissive surfaces are counted (never NEE'd).
// Surface vertices: sun NEE (uniform cone) and one local light (candidate grid) with MIS; BSDF continuation.
#include "unx/reference/PathTracer.h"

#include "Atmosphere.h"
#include "Bsdf.h"
#include "HoldRecord.h"
#include "Lights.h"
#include "RtScene.h"
#include "Sampler.h"

#include "unx/core/Jobs.h"
#include "unx/core/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#include <windows.h>

namespace unx::reference
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kForceScale = 16.0f;
constexpr uint32_t kMaxBounces = 4096;  // safety only: Russian roulette ends paths long before (counted if hit)

Double3 toD(float3 p) { return { p.x, p.y, p.z }; }
float3 toF(const Double3& p) { return { (float)p.x, (float)p.y, (float)p.z }; }

struct Quat
{
    float w, x, y, z;
};
Quat fromBasis(float3 right, float3 up, float3 back)
{
    // Rotation matrix columns = right, up, back (camera looks along -back).
    const float m00 = right.x, m11 = up.y, m22 = back.z;
    const float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0)
    {
        const float s = std::sqrt(tr + 1) * 2;
        q = { 0.25f * s, (up.z - back.y) / s, (back.x - right.z) / s, (right.y - up.x) / s };
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = { (up.z - back.y) / s, 0.25f * s, (up.x + right.y) / s, (back.x + right.z) / s };
    }
    else if (m11 > m22)
    {
        const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = { (back.x - right.z) / s, (up.x + right.y) / s, 0.25f * s, (back.y + up.z) / s };
    }
    else
    {
        const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = { (right.y - up.x) / s, (back.x + right.z) / s, (back.y + up.z) / s, 0.25f * s };
    }
    return q;
}
float3 rotate(const Quat& q, float3 v)
{
    const float3 u{ q.x, q.y, q.z };
    const float3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
Quat slerp(Quat a, Quat b, float t)
{
    float c = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (c < 0)
    {
        b = { -b.w, -b.x, -b.y, -b.z };
        c = -c;
    }
    float ka = 1 - t, kb = t;
    if (c < 0.9995f)
    {
        const float th = std::acos(c), s = std::sin(th);
        ka = std::sin((1 - t) * th) / s;
        kb = std::sin(t * th) / s;
    }
    Quat q{ ka * a.w + kb * b.w, ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z };
    const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return { q.w / n, q.x / n, q.y / n, q.z / n };
}
Quat cameraQuat(float3 forward, float3 up)
{
    const float3 back = normalize(-forward);
    const float3 right = normalize(cross(up, back));
    return fromBasis(right, cross(back, right), back);
}
} // namespace

ResolvedCamera resolveCamera(const scene::Scene& s, const CameraSelection& sel)
{
    ResolvedCamera c;
    auto fromCamera = [&](const scene::Camera& cam) {
        c.position = cam.position;
        c.forward = normalize(cam.forward);
        c.up = normalize(cam.up);
        c.verticalFov = cam.verticalFov;
        c.nearPlane = cam.nearPlane;
        c.ev100 = cam.ev100;
    };
    if (!sel.camera.empty())
    {
        for (const scene::Camera& cam : s.cameras)
            if (cam.name == sel.camera)
            {
                fromCamera(cam);
                c.time = sel.time;
                return c;
            }
        fail("reference: scene '%s' has no camera '%s'", s.name.c_str(), sel.camera.c_str());
    }
    for (const scene::CameraPath& p : s.paths)
    {
        if (p.name != sel.path) continue;
        // Lens and exposure: the camera whose name the path carries ("<camera>_static"), else the first camera.
        const scene::Camera* lens = s.cameras.empty() ? nullptr : &s.cameras[0];
        for (const scene::Camera& cam : s.cameras)
            if (p.name == cam.name || p.name == cam.name + "_static") lens = &cam;
        if (lens) fromCamera(*lens);
        const float t = std::clamp(sel.time, p.keys.front().time, p.keys.back().time);
        size_t k = 0;
        while (k + 2 < p.keys.size() && p.keys[k + 1].time < t) ++k;
        const scene::CameraKey &a = p.keys[k], &b = p.keys[std::min(k + 1, p.keys.size() - 1)];
        const float f = b.time > a.time ? (t - a.time) / (b.time - a.time) : 0.0f;
        c.position = a.position + (b.position - a.position) * f;
        const Quat q = slerp(cameraQuat(a.forward, a.up), cameraQuat(b.forward, b.up), f);
        c.forward = normalize(rotate(q, { 0, 0, -1 }));
        c.up = normalize(rotate(q, { 0, 1, 0 }));
        c.time = t;
        return c;
    }
    fail("reference: scene '%s' has no camera path '%s'", s.name.c_str(), sel.path.c_str());
}

struct PathTracer::Impl
{
    const scene::Scene& scene;
    float time = 0;
    AtmosphereModel atm;
    std::unique_ptr<RtScene> rt;
    LightSet lights;
    uint32_t threads = 0;
    // Sun.
    float3 sunDir;
    float sunSin2 = 0, sunSolidAngle = 0;
    double sunHalfSin = 0;  // sin(theta_s / 2)
    Rgb sunRadiance;  // at the top of the atmosphere
    float3 sunT1, sunT2;

    Impl(const scene::Scene& s, uint32_t th) : scene(s), atm(s.atmosphere), lights(s), threads(th)
    {
        sunDir = normalize(s.sun.direction);
        // 1 - cos(theta_s) ~ 1e-5 is below float resolution near 1: use 2 sin^2(theta_s / 2) in double.
        const double th0 = s.sun.angularRadius;
        sunHalfSin = std::sin(0.5 * th0);
        sunSolidAngle = (float)(2.0 * 3.14159265358979323846 * 2.0 * sunHalfSin * sunHalfSin);
        sunSin2 = (float)(std::sin(th0) * std::sin(th0));
        sunRadiance = Rgb(s.sun.color) * (float)(s.sun.illuminance / (3.14159265358979323846 * std::sin(th0) * std::sin(th0)));
        const float3 ref = std::fabs(sunDir.y) < 0.99f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
        sunT1 = normalize(cross(ref, sunDir));
        sunT2 = cross(sunDir, sunT1);
    }

    void build(float t)
    {
        if (rt && time == t) return;
        time = t;
        rt.reset();
        rt = std::make_unique<RtScene>(scene, t, threads);
    }

    struct Counters
    {
        uint64_t rays = 0, truncated = 0;
    };

    // Uniform in the cone: 1 - cos(theta) = u (1 - cos(theta_s)), i.e. sin(theta / 2) = sqrt(u) sin(theta_s / 2).
    float3 sampleSun(float u1, float u2) const
    {
        const double half = std::asin(std::sqrt((double)u1) * sunHalfSin);
        const float s = (float)std::sin(2 * half), c = (float)std::cos(2 * half), phi = 2 * kPi * u2;
        return normalize(sunDir * c + sunT1 * (s * std::cos(phi)) + sunT2 * (s * std::sin(phi)));
    }
    bool inSun(float3 d) const
    {
        const float3 x = cross(d, sunDir);
        return dot(d, sunDir) > 0 && dot(x, x) <= sunSin2;
    }

    // Sun radiance arriving at p from direction w (in the cone): atmosphere + planet + scene visibility.
    Rgb sunArriving(float3 p, float3 w, float3 offsetNormal, bool useOffset, Counters& cnt) const
    {
        const Double3 pd = toD(p);
        if (atm.groundDistance(pd, w) > 0) return {};
        const Rgb tau = atm.opticalDepthToTop(pd, w);
        if (!tau.finite()) return {};
        const float3 o = useOffset ? offsetRayOrigin(p, offsetNormal) : p;
        ++cnt.rays;
        if (rt->occluded(o, w, 0.0f, INFINITY)) return {};
        return sunRadiance * expNeg(tau);
    }

    // Local light arriving at p from a sample (visibility for shadowing lights, atmosphere along the segment).
    Rgb lightArriving(uint32_t li, float3 p, float3 offsetNormal, bool useOffset, const LightSample& ls, Counters& cnt) const
    {
        const scene::Light& l = lights.light(li);
        if (l.castShadow)
        {
            const float3 o = useOffset ? offsetRayOrigin(p, offsetNormal) : p;
            ++cnt.rays;
            if (rt->occluded(o, ls.wi, 0.0f, ls.distance * (1 - 1e-4f))) return {};
        }
        return ls.L * expNeg(atm.opticalDepth(toD(p), ls.wi, ls.distance));
    }

    // Pdf for points on a segment [0, len]. Short segments (inside the scene): uniform (the density varies by less
    // than a factor exp(len * |mu| / H_M) there). Long segments (to the sky or the planet ground): a mixture of two
    // truncated exponentials whose scales are the distances over which the ray climbs one Rayleigh / Mie scale height
    // (H / mu, capped by the curvature distance sqrt(2 R H) for grazing rays), plus 10% uniform so every point keeps a
    // positive density. The estimate always uses the exact transmittance and scattering at the sampled point, so the
    // shape only affects variance.
    struct SegmentPdf
    {
        bool uniform = true;
        double len = 0;
        double scale[2] = { 0, 0 }, norm[2] = { 0, 0 };  // norm = 1 - exp(-len / scale)
        static constexpr float kW[3] = { 0.45f, 0.45f, 0.10f };
        float pdf(double t) const
        {
            if (uniform) return (float)(1.0 / len);
            double p = kW[2] / len;
            for (int k = 0; k < 2; ++k) p += kW[k] * std::exp(-t / scale[k]) / (scale[k] * norm[k]);
            return (float)p;
        }
        float sample(float u, float& pdfOut) const
        {
            double t;
            if (uniform) t = u * len;
            else if (u < kW[0] + kW[1])
            {
                const int k = u < kW[0] ? 0 : 1;
                const double v = k == 0 ? u / kW[0] : (u - kW[0]) / kW[1];
                t = -scale[k] * std::log1p(-std::min(v, 0.99999994) * norm[k]);
            }
            else t = (u - kW[0] - kW[1]) / kW[2] * len;
            t = std::clamp(t, 0.0, len);
            pdfOut = pdf(t);
            return (float)t;
        }
    };
    SegmentPdf segmentPdf(const Double3& o, float3 d, double len) const
    {
        SegmentPdf p;
        p.len = len;
        if (len < AtmosphereModel::kShortSegment) return p;
        p.uniform = false;
        const double R = atm.bottomRadius(), oy = o.y + R;
        const double r = std::sqrt(o.x * o.x + oy * oy + o.z * o.z);
        const double mu = (o.x * d.x + oy * d.y + o.z * d.z) / r;
        const double H[2] = { scene.atmosphere.rayleighScaleHeight, scene.atmosphere.mieScaleHeight };
        for (int k = 0; k < 2; ++k)
        {
            const double curvature = std::sqrt(2.0 * R * H[k]);
            p.scale[k] = mu > 1e-9 ? std::min(H[k] / mu, curvature) : curvature;
            p.norm[k] = -std::expm1(-len / p.scale[k]);
        }
        return p;
    }

    Rgb radiance(float3 origin, float3 dir, float tnear, Sampler& smp, Counters& cnt, uint32_t rrStart) const;
    bool forced = true;

    // --- Sun caustics. Paths camera -> x -> y_k ... y_1 -> sun with x not smooth, every y smooth (GGX alpha <= 0.02 by
    // material, no roughness texture, Standard class) and y_1 (the vertex that sees the sun) on a rigid instance carry
    // the sun's image in near-mirror surfaces onto rough ones. A camera path reaches them only when a BSDF sample from
    // x happens to leave through the reflected 0.5 deg sun disk, so 4096 spp leave them as fireflies (city_block:
    // windows on the road and the facades). They are estimated by light tracing instead (sun -> y_1 ... y_k -> x,
    // connected to the pinhole camera) and the camera paths drop exactly this class: a partition of the path space,
    // so the image keeps its expectation. The class is decided from the same material predicate and instance flag on
    // both sides.
    static constexpr float kSmoothAlpha = 0.02f;
    bool caustics = false;             // light tracing active (emission set non-empty and RenderSettings::sunCaustics)
    std::vector<uint8_t> smoothMat;    // per scene material
    struct EmitTri
    {
        float3 p0, e1, e2;
        uint32_t instance, triangle;
    };
    std::vector<EmitTri> emitTris;     // smooth triangles of rigid instances, world space
    std::vector<double> emitCdf;       // cumulative area
    double emitArea = 0;
    bool smooth(const Surface& s) const { return s.material < smoothMat.size() && smoothMat[s.material] != 0; }
    void buildCaustics();
    // One light path; adds its camera connection (times scale) to sum (W x H x 3).
    void traceCaustic(const ResolvedCamera& cam, uint32_t W, uint32_t H, Pcg32& rng, double* sum, double scale, Counters& cnt) const;
    uint32_t orderMin = 0, orderMax = 0xFFFFFFFFu;  // RenderSettings::volumeOrderMin/Max
    uint32_t surfMin = 0, surfMax = 0xFFFFFFFFu;    // RenderSettings::surfaceOrderMin/Max
    float orderWeight(uint32_t kVol, uint32_t kSurf) const
    {
        return kVol >= orderMin && kVol <= orderMax && kSurf >= surfMin && kSurf <= surfMax ? 1.0f : 0.0f;
    }
    // Direct light (sun + one local light) scattered at y towards -d, per unit throughput.
    Rgb mediumNee(float3 yf, const AtmosphereModel::Coefficients& c, float3 d, Sampler& smp, Counters& cnt, LightCandidates& cands) const;
};

Rgb PathTracer::Impl::mediumNee(float3 yf, const AtmosphereModel::Coefficients& c, float3 d, Sampler& smp, Counters& cnt, LightCandidates& cands) const
{
    Rgb L;
    float u1, u2;
    smp.get2D(u1, u2);
    const float3 ws = sampleSun(u1, u2);
    const float cs = dot(ws, d);
    const Rgb phaseS = c.scatteringRayleigh * atm.phaseRayleigh(cs) + c.scatteringMie * atm.phaseMie(cs);
    if (!phaseS.isZero())
    {
        const Rgb Ls = sunArriving(yf, ws, {}, false, cnt);
        if (!Ls.isZero()) L += phaseS * Ls * sunSolidAngle;
    }
    const float uSel = smp.get1D();
    smp.get2D(u1, u2);
    if (!lights.empty())
    {
        lights.gather(yf, cands);
        if (!cands.lights.empty())
        {
            float pSel;
            const uint32_t li = cands.lights[LightSet::choose(cands, uSel, pSel)];
            LightSample ls;
            if (lights.sample(li, yf, u1, u2, ls))
            {
                const float cl = dot(ls.wi, d);
                const Rgb phaseL = c.scatteringRayleigh * atm.phaseRayleigh(cl) + c.scatteringMie * atm.phaseMie(cl);
                const Rgb Ll = lightArriving(li, yf, {}, false, ls, cnt);
                if (!Ll.isZero()) L += phaseL * Ll * (1.0f / (pSel * ls.pdf));
            }
        }
    }
    return L;
}

bool sunCausticMaterial(const scene::Material& mt, float smoothAlpha)
{
    return mt.cls == scene::MaterialClass::Standard && mt.roughMetalTexture == scene::kNone && scene::model::alphaFromRoughness(mt.roughness) <= smoothAlpha;
}
bool sunCausticMaterial(const scene::Material& mt) { return sunCausticMaterial(mt, PathTracer::Impl::kSmoothAlpha); }

bool hasSunCausticSurfaces(const scene::Scene& scene)
{
    for (const scene::Instance& in : scene.instances)
    {
        const scene::Mesh& mesh = scene.meshes[in.mesh];
        for (size_t si = 0; si < mesh.submeshes.size(); ++si)
        {
            const uint32_t mat = in.materialOverrides.empty() ? mesh.submeshes[si].material : in.materialOverrides[si];
            if (mat < scene.materials.size() && sunCausticMaterial(scene.materials[mat], PathTracer::Impl::kSmoothAlpha)) return true;
        }
    }
    return false;
}

void PathTracer::Impl::buildCaustics()
{
    smoothMat.assign(scene.materials.size(), 0);
    for (size_t m = 0; m < scene.materials.size(); ++m)
        if (sunCausticMaterial(scene.materials[m], kSmoothAlpha)) smoothMat[m] = 1;
    emitTris.clear();
    emitCdf.clear();
    emitArea = 0;
    for (uint32_t i = 0; i < (uint32_t)scene.instances.size(); ++i)
    {
        if (rt->deformed(i)) continue;
        const scene::Instance& in = scene.instances[i];
        const scene::Mesh& mesh = scene.meshes[in.mesh];
        for (size_t si = 0; si < mesh.submeshes.size(); ++si)
        {
            const scene::Submesh& sub = mesh.submeshes[si];
            const uint32_t mat = in.materialOverrides.empty() ? sub.material : in.materialOverrides[si];
            if (mat >= smoothMat.size() || !smoothMat[mat]) continue;
            for (uint32_t k = sub.indexOffset; k + 2 < sub.indexOffset + sub.indexCount; k += 3)
            {
                const float3 p0 = in.transform.transformPoint(mesh.positions[mesh.indices[k]]);
                const float3 p1 = in.transform.transformPoint(mesh.positions[mesh.indices[k + 1]]);
                const float3 p2 = in.transform.transformPoint(mesh.positions[mesh.indices[k + 2]]);
                const float3 c = cross(p1 - p0, p2 - p0);
                const double area = 0.5 * std::sqrt((double)dot(c, c));
                if (!(area > 0)) continue;
                emitTris.push_back({ p0, p1 - p0, p2 - p0, i, k / 3 });
                emitArea += area;
                emitCdf.push_back(emitArea);
            }
        }
    }
}

void PathTracer::Impl::traceCaustic(const ResolvedCamera& cam, uint32_t W, uint32_t H, Pcg32& rng, double* sum, double scale, Counters& cnt) const
{
    // Emission: a point on the smooth set (uniform in area) lit by a direction in the sun cone (uniform in solid angle).
    const double ua = rng.uniform() * emitArea;
    const size_t ti = std::min((size_t)(std::upper_bound(emitCdf.begin(), emitCdf.end(), ua) - emitCdf.begin()), emitTris.size() - 1);
    const EmitTri& et = emitTris[ti];
    const float r1 = rng.uniform(), r2 = rng.uniform(), su = std::sqrt(r1);
    Hit h;
    h.instance = et.instance;
    h.triangle = et.triangle;
    h.u = 1 - su;
    h.v = r2 * su;
    const float u1 = rng.uniform(), u2 = rng.uniform();
    if (!rt->alphaOpaque(h.instance, h.triangle, h.u, h.v)) return;
    const float3 ws = sampleSun(u1, u2);
    Surface s = rt->surface(h, -ws);
    if (!s.frontFacing || !smooth(s)) return;
    const float nsl = dot(s.ns, ws);
    if (dot(s.ng, ws) <= 0 || nsl <= 0) return;
    const Rgb Ls = sunArriving(s.p, ws, s.ng, true, cnt);
    if (Ls.isZero()) return;
    Rgb beta = Ls * (float)(emitArea * sunSolidAngle * nsl);
    float3 l = ws;  // towards the light
    for (int depth = 0; depth < 16; ++depth)
    {
        // At smooth vertex s: sample the camera-side direction v; weight f(v, l) |l.ns| |v.ng| / (|l.ng| pdf(v)).
        const Bsdf bl(s, l);
        BsdfSample bs;
        if (!bl.sample(rng.uniform(), rng.uniform(), rng.uniform(), bs)) return;
        const float3 v = bs.wi;
        const float gv = dot(s.ng, v);
        if (gv <= 0 || dot(s.ns, v) <= 0) return;
        const Rgb f = Bsdf(s, v).eval(l);
        if (f.isZero()) return;
        beta *= f * (std::fabs(dot(l, s.ns)) / std::fabs(dot(l, s.ng)) * gv / bs.pdf);
        // Next vertex (scene surface or planet ground), transmittance along the segment (no medium event: that is
        // another path class, left to the camera paths).
        const float3 o = offsetRayOrigin(s.p, s.ng);
        Hit hn;
        ++cnt.rays;
        Surface x;
        bool lambert = false;
        double len;
        const bool surf = rt->intersect(o, v, 0.0f, INFINITY, kMaskAll, hn);
        if (surf)
        {
            len = hn.t;
            x = rt->surface(hn, v);
            if (!x.frontFacing) return;
        }
        else
        {
            const Double3 od = toD(o);
            const double g = atm.groundDistance(od, v);
            if (!(g > 0)) return;
            len = g;
            const Double3 gp{ od.x + v.x * g, od.y + v.y * g, od.z + v.z * g };
            const double gy = gp.y + atm.bottomRadius();
            const double inv = 1.0 / std::sqrt(gp.x * gp.x + gy * gy + gp.z * gp.z);
            x.p = toF(gp);
            x.ng = x.ns = float3{ (float)(gp.x * inv), (float)(gy * inv), (float)(gp.z * inv) };
            x.bsdf.baseColor = scene.atmosphere.groundAlbedo;
            lambert = true;
        }
        beta *= expNeg(atm.opticalDepth(toD(o), v, len));
        l = -v;
        if (!lambert && smooth(x))
        {
            s = x;
            continue;
        }
        // First rough vertex x: connect to the pinhole camera.
        const float3 toCam = cam.position - x.p;
        const float d2 = dot(toCam, toCam), dist = std::sqrt(d2);
        const float3 wc = toCam * (1.0f / dist), dc = -wc;
        const float3 fw = normalize(cam.forward), right = normalize(cross(fw, cam.up)), up = cross(right, fw);
        const float zc = dot(dc, fw);
        if (zc <= 1e-6f) return;
        const float th = std::tan(0.5f * cam.verticalFov), aspect = (float)W / (float)H;
        const float nx = dot(dc, right) / (zc * th * aspect), ny = dot(dc, up) / (zc * th);
        const float px = (nx + 1) * 0.5f * W, py = (1 - ny) * 0.5f * H;
        if (!(px >= 0 && px < W && py >= 0 && py < H)) return;
        const float tn = cam.nearPlane / zc;
        if (dist <= tn) return;
        Surface xc = x;  // x as the camera sees it
        if (surf)
        {
            xc = rt->surface(hn, dc);
            if (!xc.frontFacing) return;
        }
        else if (dot(xc.ng, wc) <= 0) return;
        const float gc = std::fabs(dot(wc, xc.ng));
        const Rgb fx = Bsdf(xc, wc, lambert).eval(l);
        if (fx.isZero()) return;
        Hit hv;
        ++cnt.rays;
        if (rt->intersect(cam.position, dc, tn, dist * (1 - 1e-4f), kMaskAll, hv)) return;
        if (!surf && atm.groundDistance(toD(cam.position), dc) < dist * (1 - 1e-4)) return;
        const Rgb T = expNeg(atm.opticalDepth(toD(cam.position), dc, dist));
        const float Ap = (2 * th * aspect / W) * (2 * th / H);
        const Rgb val = beta * fx * T *
                        (std::fabs(dot(l, xc.ns)) / std::fabs(dot(l, xc.ng)) * gc / (d2 * Ap * zc * zc * zc) * orderWeight(0, (uint32_t)depth + 2) * (float)scale);
        if (!val.finite()) return;
        const size_t pi = (size_t)std::min((uint32_t)py, H - 1) * W + std::min((uint32_t)px, W - 1);
        std::atomic_ref<double>(sum[3 * pi]).fetch_add(val.r);
        std::atomic_ref<double>(sum[3 * pi + 1]).fetch_add(val.g);
        std::atomic_ref<double>(sum[3 * pi + 2]).fetch_add(val.b);
        return;
    }
}

Rgb PathTracer::Impl::radiance(float3 origin, float3 dir, float tnear, Sampler& smp, Counters& cnt, uint32_t rrStart) const
{
    enum class Prev { Camera, Surface, Medium };
    Rgb L, beta(1.0f);
    float3 o = origin, d = dir;
    float tmin = tnear;
    Prev prev = Prev::Camera;
    float prevBsdfPdf = 0;
    float3 prevPos{};
    LightCandidates cands, prevCands;
    uint32_t nVol = 0;  // atmosphere scattering events so far (order diagnostics)
    // Sun caustic class (see Impl): chain = the path so far is camera -> rough x -> smooth vertices, no medium event.
    uint32_t surfVerts = 0;
    bool chain = false, dropSun = false;

    for (uint32_t bounce = 0;; ++bounce)
    {
        if (bounce >= kMaxBounces)
        {
            ++cnt.truncated;
            break;
        }
        // --- trace the segment
        Hit hit;
        ++cnt.rays;
        const bool surf = rt->intersect(o, d, tmin, INFINITY, kMaskAll, hit);
        const Double3 od = toD(o);
        enum class End { Surface, Ground, Space } end;
        double segLen;
        if (surf)
        {
            end = End::Surface;
            segLen = hit.t;
        }
        else
        {
            const double g = atm.groundDistance(od, d);
            if (g > 0)
            {
                end = End::Ground;
                segLen = g;
            }
            else
            {
                // Below the planet surface (scene valleys) heading further down with no scene geometry: the ray is
                // inside the planet's solid ground, which absorbs it.
                if (atm.altitude(od) < 0 && od.x * d.x + (od.y + atm.bottomRadius()) * d.y + od.z * d.z < 0) break;
                end = End::Space;
                segLen = atm.topDistance(od, d);
                if (!(segLen > 0)) segLen = 0;
            }
        }

        // --- 1. analytic area-light emission along the segment (after a surface vertex)
        if (prev == Prev::Surface)
        {
            for (size_t k = 0; k < prevCands.lights.size(); ++k)
            {
                const uint32_t li = prevCands.lights[k];
                const scene::Light& l = lights.light(li);
                if (!l.castShadow) continue;  // unshadowed lights are estimated by NEE alone
                float t, pdfSA;
                Rgb Le;
                if (!lights.intersect(li, prevPos, d, (float)segLen, t, Le, pdfSA)) continue;
                const float pSel = (prevCands.cumulative[k] - (k ? prevCands.cumulative[k - 1] : 0.0f)) / prevCands.total;
                const float w = powerHeuristic(prevBsdfPdf, pSel * pdfSA);
                L += beta * expNeg(atm.opticalDepth(od, d, t)) * Le * (w * orderWeight(nVol, surfVerts));
            }
        }

        // --- 2./3. atmosphere on the segment
        Rgb tauSeg = end == End::Space ? atm.opticalDepthToTop(od, d) : atm.opticalDepth(od, d, segLen);
        if (!tauSeg.finite()) tauSeg = atm.opticalDepth(od, d, segLen);
        const Rgb Tseg = expNeg(tauSeg);
        const float Tavg = Tseg.avg();
        const SegmentPdf spdf = segmentPdf(od, d, segLen);
        if (segLen > 0)
        {
            const float pForce = forced ? std::min(1.0f, kForceScale * (1 - Tavg)) : 0.0f;
            if (pForce > 0 && smp.get1D() < pForce)
            {
                float pt;
                const float t = spdf.sample(smp.get1D(), pt);
                const Double3 y{ od.x + d.x * t, od.y + d.y * t, od.z + d.z * t };
                const Rgb T = expNeg(atm.opticalDepth(od, d, t));
                const AtmosphereModel::Coefficients c = atm.at(atm.altitude(y));
                L += beta * T * mediumNee(toF(y), c, d, smp, cnt, cands) * (orderWeight(nVol + 1, surfVerts) / (pt * pForce));
            }
            else if (forced)
            {
                smp.get1D();
                float a, b;
                smp.get2D(a, b);
                smp.get1D();
                smp.get2D(a, b);
            }
            const float pScatter = 1 - Tavg;
            if (pScatter > 0 && smp.get1D() < pScatter)
            {
                float pt;
                const float t = spdf.sample(smp.get1D(), pt);
                const Double3 y{ od.x + d.x * t, od.y + d.y * t, od.z + d.z * t };
                const Rgb T = expNeg(atm.opticalDepth(od, d, t));
                const AtmosphereModel::Coefficients c = atm.at(atm.altitude(y));
                float u1, u2;
                smp.get2D(u1, u2);
                const float uMix = smp.get1D();
                // Absorption-only point: this branch carries zero weight (the path ends; unbiased).
                if (c.scatteringRayleigh.max() + c.scatteringMie.max() <= 0) break;
                if (!forced) L += beta * T * mediumNee(toF(y), c, d, smp, cnt, cands) * (orderWeight(nVol + 1, surfVerts) / (pt * pScatter));
                float pdfDir;
                const float3 w = atm.samplePhase(d, c.scatteringRayleigh.avg(), c.scatteringMie.avg(), uMix, u1, u2, pdfDir);
                const float cw = dot(w, d);
                const Rgb phase = c.scatteringRayleigh * atm.phaseRayleigh(cw) + c.scatteringMie * atm.phaseMie(cw);
                beta *= T * phase * (1.0f / (pt * pScatter * pdfDir));
                o = toF(y);
                d = w;
                tmin = 0;
                prev = Prev::Medium;
                chain = false;
                dropSun = false;
                if (++nVol > orderMax) break;  // no later contribution can be inside the order window
                if (bounce + 1 >= rrStart)
                {
                    const float q = std::min(1.0f, beta.max());
                    if (!(smp.get1D() < q)) break;
                    beta *= 1.0f / q;
                }
                continue;
            }
            beta *= Tseg * (1.0f / std::max(Tavg, 1e-30f));
        }

        // --- segment end
        if (end == End::Space)
        {
            if (prev != Prev::Medium && !(prev == Prev::Surface && dropSun) && inSun(d))
            {
                const float w = prev == Prev::Camera ? 1.0f : powerHeuristic(prevBsdfPdf, 1.0f / sunSolidAngle);
                L += beta * sunRadiance * (w * orderWeight(nVol, surfVerts));
            }
            break;
        }
        Surface s;
        bool lambert = false;
        if (end == End::Surface)
        {
            s = rt->surface(hit, d);
            if (!s.emission.isZero()) L += beta * s.emission * orderWeight(nVol, surfVerts);
            if (!s.frontFacing) break;  // back of a one-sided surface: the BRDF is zero for this view
        }
        else
        {
            // Planet ground (outside the scene geometry): Lambert with the atmosphere's ground albedo.
            const Double3 g{ od.x + d.x * segLen, od.y + d.y * segLen, od.z + d.z * segLen };
            const double gy = g.y + atm.bottomRadius();
            const double inv = 1.0 / std::sqrt(g.x * g.x + gy * gy + g.z * g.z);
            s.p = toF(g);
            s.ng = s.ns = float3{ (float)(g.x * inv), (float)(gy * inv), (float)(g.z * inv) };
            s.bsdf.baseColor = scene.atmosphere.groundAlbedo;
            lambert = true;
        }
        const float3 wo = -d;
        const Bsdf bsdf(s, wo, lambert);
        {
            const bool isSmooth = !lambert && smooth(s);
            ++surfVerts;
            if (surfVerts == 1) chain = prev == Prev::Camera && !isSmooth;
            else if (chain) chain = isSmooth;
            // Sun seen from this vertex (NEE here, or a BSDF sample from here reaching the disk) belongs to the light
            // tracer when the chain holds and this smooth vertex is on a rigid instance.
            dropSun = caustics && chain && surfVerts >= 2 && isSmooth && end == End::Surface && !rt->deformed(hit.instance);
            if (surfVerts > surfMax) break;  // every later contribution has more surface events than the window
        }

        // Sun NEE.
        {
            float u1, u2;
            smp.get2D(u1, u2);
            const float3 ws = sampleSun(u1, u2);
            const Rgb f = bsdf.eval(ws);
            if (!f.isZero())
            {
                const float3 side = dot(s.ng, ws) >= 0 ? s.ng : -s.ng;
                const Rgb Ls = sunArriving(s.p, ws, side, true, cnt);
                if (!Ls.isZero())
                {
                    const float pl = 1.0f / sunSolidAngle;
                    if (!dropSun) L += beta * f * Ls * (bsdf.cosine(ws) * powerHeuristic(pl, bsdf.pdf(ws)) * orderWeight(nVol, surfVerts) / pl);
                }
            }
        }
        // Local light NEE.
        lights.gather(s.p, cands);
        {
            const float uSel = smp.get1D();
            float u1, u2;
            smp.get2D(u1, u2);
            if (!cands.lights.empty())
            {
                float pSel;
                const uint32_t li = cands.lights[LightSet::choose(cands, uSel, pSel)];
                LightSample ls;
                if (lights.sample(li, s.p, u1, u2, ls))
                {
                    const Rgb f = bsdf.eval(ls.wi);
                    if (!f.isZero())
                    {
                        const float3 side = dot(s.ng, ls.wi) >= 0 ? s.ng : -s.ng;
                        const Rgb Ll = lightArriving(li, s.p, side, true, ls, cnt);
                        if (!Ll.isZero())
                        {
                            const float pl = pSel * ls.pdf;
                            const bool mis = !ls.delta && lights.light(li).castShadow;
                            const float w = mis ? powerHeuristic(pl, bsdf.pdf(ls.wi)) : 1.0f;
                            L += beta * f * Ll * (bsdf.cosine(ls.wi) * w * orderWeight(nVol, surfVerts) / pl);
                        }
                    }
                }
            }
        }
        // Continue.
        const float uLobe = smp.get1D();
        float u1, u2;
        smp.get2D(u1, u2);
        BsdfSample bs;
        if (!bsdf.sample(uLobe, u1, u2, bs)) break;
        beta *= bs.f * (bsdf.cosine(bs.wi) / bs.pdf);
        prevBsdfPdf = bs.pdf;
        prevPos = s.p;
        std::swap(prevCands, cands);
        prev = Prev::Surface;
        o = offsetRayOrigin(s.p, dot(s.ng, bs.wi) >= 0 ? s.ng : -s.ng);
        d = bs.wi;
        tmin = 0;
        if (bounce + 1 >= rrStart)
        {
            const float q = std::min(1.0f, beta.max());
            if (!(smp.get1D() < q)) break;
            beta *= 1.0f / q;
        }
        if (beta.isZero()) break;
    }
    return L;
}

namespace
{
// Pauses render workers while any of the given hold files is active (HoldRecord.h: a live timing GPU lock record, or
// a manual marker). One watcher thread polls every 200 ms; workers only read an atomic flag.
class PauseGate
{
public:
    explicit PauseGate(std::vector<std::filesystem::path> files) : m_files(std::move(files))
    {
        if (m_files.empty()) return;
        m_paused.store(held());  // pause before the first row if a hold is already in place
        m_thread = std::thread([this] {
            bool was = m_paused.load();
            auto pauseStart = std::chrono::steady_clock::now();
            if (was) logf("reference: hold in place (%s) - render paused\n", m_reason.c_str());
            while (!m_stop.load())
            {
                const bool now = held();
                if (now && !was)
                {
                    pauseStart = std::chrono::steady_clock::now();
                    logf("reference: hold in place (%s) - render paused\n", m_reason.c_str());
                }
                if (!now && was)
                {
                    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - pauseStart).count();
                    m_pausedMs.fetch_add((uint64_t)(sec * 1000));
                    logf("reference: hold released - resumed after %.1f s\n", sec);
                }
                m_paused.store(now);
                was = now;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (was) m_pausedMs.fetch_add((uint64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now() - pauseStart).count() * 1000));
            m_paused.store(false);
        });
    }
    ~PauseGate()
    {
        m_stop.store(true);
        if (m_thread.joinable()) m_thread.join();
    }
    PauseGate(const PauseGate&) = delete;
    PauseGate& operator=(const PauseGate&) = delete;
    void wait() const
    {
        while (m_paused.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    uint64_t pausedMs() const { return m_pausedMs.load(); }

private:
    bool held()
    {
        for (const std::filesystem::path& f : m_files)
        {
            if (!holdActive(f)) continue;
            m_reason = f.string();
            return true;
        }
        return false;
    }
    std::vector<std::filesystem::path> m_files;
    std::string m_reason;
    std::atomic<bool> m_paused{ false }, m_stop{ false };
    std::atomic<uint64_t> m_pausedMs{ 0 };
    std::thread m_thread;
};
} // namespace

void waitWhileHeld(const std::vector<std::filesystem::path>& files)
{
    const PauseGate gate(files);
    gate.wait();
}

PathTracer::PathTracer(const scene::Scene& scene, uint32_t threads) : m_impl(std::make_unique<Impl>(scene, threads)) {}
PathTracer::~PathTracer() = default;

namespace
{
struct CheckpointHeader
{
    char magic[8] = { 'U', 'N', 'X', 'R', 'E', 'F', 'C', '1' };
    uint32_t width = 0, height = 0, spp = 0, samplesDone = 0;
    uint64_t seed = 0;
    uint64_t paths = 0, rays = 0, truncated = 0, nans = 0;
    double seconds = 0;
    char identity[256] = {};
};

float3 cameraRay(const ResolvedCamera& c, uint32_t w, uint32_t h, float px, float py)
{
    const float3 f = normalize(c.forward);
    const float3 right = normalize(cross(f, c.up));
    const float3 up = cross(right, f);
    const float th = std::tan(0.5f * c.verticalFov), aspect = (float)w / (float)h;
    const float nx = px / w * 2 - 1, ny = 1 - py / h * 2;
    return normalize(f + right * (nx * th * aspect) + up * (ny * th));
}

// Thin lens (the GPU tracer's Common.hlsli rule): the lens point o = position + lensRadius x (concentric disk point) in the
// (right, up) plane; the ray from o through the point where the pinhole ray meets the plane of focus.
void thinLens(const ResolvedCamera& c, float3 pinholeDir, float u1, float u2, float3& origin, float3& dir)
{
    const float3 f = normalize(c.forward);
    const float3 right = normalize(cross(f, c.up));
    const float3 up = cross(right, f);
    const float a = 2 * u1 - 1, b = 2 * u2 - 1;
    float r = 0, phi = 0;
    if (a != 0 || b != 0)
    {
        constexpr float kQuarterPi = 0.78539816f;
        if (std::fabs(a) > std::fabs(b)) r = a, phi = kQuarterPi * (b / a);
        else r = b, phi = 2 * kQuarterPi - kQuarterPi * (a / b);
    }
    const float lensRadius = 0.5f * c.lensAperture;
    origin = c.position + right * (lensRadius * r * std::cos(phi)) + up * (lensRadius * r * std::sin(phi));
    const float3 focal = c.position + pinholeDir * (c.lensFocus / dot(pinholeDir, f));
    dir = normalize(focal - origin);
}
} // namespace

RenderOutput PathTracer::render(const ResolvedCamera& cam, const RenderSettings& st, const Progress& progress)
{
    Impl& im = *m_impl;
    if (st.width == 0 || st.height == 0) fail("reference: empty resolution");
    if (st.samplesPerPixel < 2 || (st.samplesPerPixel & 1)) fail("reference: samples per pixel must be even (two halves), got %u", st.samplesPerPixel);
    if (st.russianRouletteStart == 0) fail("reference: russian roulette start bounce must be >= 1");
    if (cam.lensAperture < 0 || !std::isfinite(cam.lensAperture)) fail("reference: lens aperture %g m", cam.lensAperture);
    if (cam.lensAperture > 0 && !(cam.lensFocus > cam.nearPlane && std::isfinite(cam.lensFocus)))
        fail("reference: thin lens focus distance %g m must exceed the near plane %g m", cam.lensFocus, cam.nearPlane);
    im.build(cam.time);
    im.buildCaustics();
    im.caustics = st.sunCaustics && !im.emitTris.empty() && im.sunSolidAngle > 0 && !im.sunRadiance.isZero();
    if (cam.lensAperture > 0 && im.caustics) fail("reference: the thin lens with sun caustics (the light tracer connects to a pinhole camera)");
    im.forced = st.forcedInScattering;
    im.orderMin = st.volumeOrderMin;
    im.orderMax = st.volumeOrderMax;
    im.surfMin = st.surfaceOrderMin;
    im.surfMax = st.surfaceOrderMax;
    const uint32_t W = st.width, H = st.height, halfSpp = st.samplesPerPixel / 2;
    const size_t pixels = (size_t)W * H;
    std::vector<double> sum[2] = { std::vector<double>(pixels * 3, 0.0), std::vector<double>(pixels * 3, 0.0) };
    CheckpointHeader hdr;
    hdr.width = W;
    hdr.height = H;
    hdr.spp = st.samplesPerPixel;
    hdr.seed = st.seed;
    std::snprintf(hdr.identity, sizeof hdr.identity, "%s|%.9g,%.9g,%.9g|%.9g,%.9g,%.9g|%.9g,%.9g,%.9g|%.9g|%.9g|%.9g|%.9g|rr%u", im.scene.name.c_str(), cam.position.x,
                  cam.position.y, cam.position.z, cam.forward.x, cam.forward.y, cam.forward.z, cam.up.x, cam.up.y, cam.up.z, cam.verticalFov, cam.nearPlane, cam.ev100,
                  cam.time, st.russianRouletteStart);
    uint32_t done = 0;  // samples per half already accumulated
    RenderStats stats;
    if (!st.checkpoint.empty() && std::filesystem::exists(st.checkpoint))
    {
        std::ifstream f(st.checkpoint, std::ios::binary);
        CheckpointHeader have;
        f.read(reinterpret_cast<char*>(&have), sizeof have);
        if (f && std::memcmp(have.magic, hdr.magic, 8) == 0 && have.width == W && have.height == H && have.spp == hdr.spp && have.seed == hdr.seed &&
            std::strcmp(have.identity, hdr.identity) == 0)
        {
            f.read(reinterpret_cast<char*>(sum[0].data()), (std::streamsize)(sum[0].size() * sizeof(double)));
            f.read(reinterpret_cast<char*>(sum[1].data()), (std::streamsize)(sum[1].size() * sizeof(double)));
            if (f)
            {
                done = have.samplesDone / 2;
                stats.paths = have.paths;
                stats.rays = have.rays;
                stats.truncatedPaths = have.truncated;
                stats.nanSamples = have.nans;
                stats.seconds = have.seconds;
                logf("reference: resumed %s at %u spp\n", st.checkpoint.string().c_str(), have.samplesDone);
            }
            else
            {
                std::fill(sum[0].begin(), sum[0].end(), 0.0);
                std::fill(sum[1].begin(), sum[1].end(), 0.0);
            }
        }
    }

    const uint32_t tile = 16, tilesX = (W + tile - 1) / tile, tilesY = (H + tile - 1) / tile;
    const float tnear0 = cam.nearPlane;
    auto lastCheckpoint = std::chrono::steady_clock::now();
    const PauseGate gate(st.pauseWhileExists);
    while (done < halfSpp)
    {
        const uint32_t begin = done, end = std::min(halfSpp, done + std::max(1u, st.samplesPerPass));
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t paused0 = gate.pausedMs();
        std::atomic<uint64_t> rays{ 0 }, truncated{ 0 }, nans{ 0 };
        Jobs::instance().parallelFor(tilesX * tilesY, [&](uint32_t ti) {
            const uint32_t tx = ti % tilesX, ty = ti / tilesX;
            Impl::Counters cnt;
            uint64_t localNans = 0;
            for (uint32_t y = ty * tile; y < std::min(H, (ty + 1) * tile); ++y)
            {
                gate.wait();
                for (uint32_t x = tx * tile; x < std::min(W, (tx + 1) * tile); ++x)
                {
                    const size_t pi = (size_t)y * W + x;
                    for (uint32_t half = 0; half < 2; ++half)
                    {
                        const uint32_t pixelSeed = hashCombine(hashCombine((uint32_t)st.seed ^ (uint32_t)(st.seed >> 32), half), (uint32_t)pi);
                        double acc[3] = { 0, 0, 0 };
                        for (uint32_t si = begin; si < end; ++si)
                        {
                            Sampler smp(pixelSeed, si);
                            float jx, jy;
                            smp.get2D(jx, jy);
                            float3 dir = cameraRay(cam, W, H, x + jx, y + jy), origin = cam.position;
                            if (cam.lensAperture > 0)  // (pinhole renders keep their sample stream: bitwise as before)
                            {
                                float u1, u2;
                                smp.get2D(u1, u2);
                                thinLens(cam, dir, u1, u2, origin, dir);
                            }
                            const float tn = tnear0 / std::max(dot(dir, normalize(cam.forward)), 1e-3f);
                            const Rgb v = im.radiance(origin, dir, tn, smp, cnt, st.russianRouletteStart);
                            if (!v.finite())
                            {
                                ++localNans;
                                continue;
                            }
                            acc[0] += v.r;
                            acc[1] += v.g;
                            acc[2] += v.b;
                        }
                        sum[half][3 * pi] += acc[0];
                        sum[half][3 * pi + 1] += acc[1];
                        sum[half][3 * pi + 2] += acc[2];
                    }
                }
            }
            rays += cnt.rays;
            truncated += cnt.truncated;
            nans += localNans;
        });
        // Sun caustics: W x H light paths per sample per half, splatted into the same sums with weight 1 / (W H) so the
        // final division by the sample count makes them an average over W H spp light paths.
        if (im.caustics)
        {
            const uint64_t perHalf = (uint64_t)pixels * (end - begin), chunk = 4096;
            const uint32_t chunks = (uint32_t)((perHalf + chunk - 1) / chunk);
            Jobs::instance().parallelFor(2 * chunks, [&](uint32_t ci) {
                gate.wait();
                const uint32_t half = ci / chunks, c = ci % chunks;
                Pcg32 rng(hashCombine(hashCombine(hashCombine((uint32_t)st.seed ^ 0xCA057105u, half), begin), c), 0xC0FFEEull + half);
                Impl::Counters cnt;
                const uint64_t n = std::min<uint64_t>(chunk, perHalf - (uint64_t)c * chunk);
                for (uint64_t k = 0; k < n; ++k) im.traceCaustic(cam, W, H, rng, sum[half].data(), 1.0 / (double)pixels, cnt);
                rays += cnt.rays;
            });
        }
        done = end;
        stats.paths += (uint64_t)pixels * 2 * (end - begin);
        stats.rays += rays;
        stats.truncatedPaths += truncated;
        stats.nanSamples += nans;
        const double pausedPass = (gate.pausedMs() - paused0) / 1000.0;
        stats.seconds += std::max(0.0, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - pausedPass);
        stats.pausedSeconds += pausedPass;
        stats.samplesDone = 2 * done;
        if (progress) progress(stats);
        const bool finished = done >= halfSpp;
        if (!st.checkpoint.empty() && !finished &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - lastCheckpoint).count() >= st.checkpointSeconds)
        {
            hdr.samplesDone = 2 * done;
            hdr.paths = stats.paths;
            hdr.rays = stats.rays;
            hdr.truncated = stats.truncatedPaths;
            hdr.nans = stats.nanSamples;
            hdr.seconds = stats.seconds;
            std::filesystem::create_directories(st.checkpoint.parent_path());
            const std::filesystem::path tmp = st.checkpoint.string() + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary);
                f.write(reinterpret_cast<const char*>(&hdr), sizeof hdr);
                f.write(reinterpret_cast<const char*>(sum[0].data()), (std::streamsize)(sum[0].size() * sizeof(double)));
                f.write(reinterpret_cast<const char*>(sum[1].data()), (std::streamsize)(sum[1].size() * sizeof(double)));
                if (!f) fail("reference: cannot write checkpoint %s", tmp.string().c_str());
            }
            std::filesystem::rename(tmp, st.checkpoint);
            lastCheckpoint = std::chrono::steady_clock::now();
        }
    }

    RenderOutput out;
    out.stats = stats;
    const float exposure = 1.0f / (1.2f * std::pow(2.0f, cam.ev100));
    for (metrics::Image* img : { &out.image, &out.halfA, &out.halfB })
    {
        img->width = W;
        img->height = H;
        img->rgb.resize(pixels * 3);
    }
    const double inv = 1.0 / halfSpp;
    for (size_t i = 0; i < pixels * 3; ++i)
    {
        const float a = (float)(sum[0][i] * inv) * exposure, b = (float)(sum[1][i] * inv) * exposure;
        out.halfA.rgb[i] = a;
        out.halfB.rgb[i] = b;
        out.image.rgb[i] = 0.5f * (a + b);
    }
    out.halvesRelMse = metrics::relMse(out.halfA, out.halfB);
    if (!st.checkpoint.empty()) std::filesystem::remove(st.checkpoint);
    return out;
}

std::vector<float> PathTracer::primaryDepths(const ResolvedCamera& cam, uint32_t W, uint32_t H)
{
    Impl& im = *m_impl;
    im.build(cam.time);
    std::vector<float> depth((size_t)W * H, INFINITY);
    const float3 f = normalize(cam.forward);
    Jobs::instance().parallelFor(H, [&](uint32_t y) {
        for (uint32_t x = 0; x < W; ++x)
        {
            const float3 dir = cameraRay(cam, W, H, x + 0.5f, y + 0.5f);
            const float tn = cam.nearPlane / std::max(dot(dir, f), 1e-3f);
            Hit h;
            if (im.rt->intersect(cam.position, dir, tn, INFINITY, kMaskAll, h)) depth[(size_t)y * W + x] = h.t * dot(dir, f);
        }
    });
    return depth;
}

std::vector<uint64_t> PathTracer::primaryIdentities(const ResolvedCamera& cam, uint32_t W, uint32_t H, uint32_t x0, uint32_t y0, uint32_t columns,
                                                    uint32_t rows)
{
    if (columns == 0 || rows == 0 || x0 >= W || y0 >= H || columns > W - x0 || rows > H - y0)
        fail("reference: census rectangle %u,%u %ux%u outside %ux%u", x0, y0, columns, rows, W, H);
    Impl& im = *m_impl;
    im.build(cam.time);
    std::vector<uint64_t> ids((size_t)columns * rows * 17);
    Jobs::instance().parallelFor(rows, [&](uint32_t r) {
        const uint32_t y = y0 + r;
        for (uint32_t c = 0; c < columns; ++c)
            for (uint32_t s = 0; s < 17; ++s)
            {
                const uint32_t x = x0 + c;
                const float jx = s < 16 ? ((s & 3) + 0.5f) / 4.0f : 0.5f, jy = s < 16 ? ((s >> 2) + 0.5f) / 4.0f : 0.5f;
                const float3 dir = cameraRay(cam, W, H, x + jx, y + jy);
                const float tn = cam.nearPlane / std::max(dot(dir, normalize(cam.forward)), 1e-3f);
                Hit h;
                ids[((size_t)r * columns + c) * 17 + s] = im.rt->intersect(cam.position, dir, tn, INFINITY, kMaskAll, h) ? ((uint64_t)h.instance << 32 | h.triangle) : kSkyIdentity;
            }
    });
    return ids;
}

std::vector<uint64_t> PathTracer::primaryIdentities(const ResolvedCamera& cam, uint32_t W, uint32_t H,
                                                    const std::vector<std::filesystem::path>& pauseWhileExists)
{
    const PauseGate gate(pauseWhileExists);
    Impl& im = *m_impl;
    im.build(cam.time);
    std::vector<uint64_t> ids((size_t)W * H * 17);
    Jobs::instance().parallelFor(H, [&](uint32_t y) {
        gate.wait();
        for (uint32_t x = 0; x < W; ++x)
            for (uint32_t s = 0; s < 17; ++s)
            {
                const float jx = s < 16 ? ((s & 3) + 0.5f) / 4.0f : 0.5f, jy = s < 16 ? ((s >> 2) + 0.5f) / 4.0f : 0.5f;
                const float3 dir = cameraRay(cam, W, H, x + jx, y + jy);
                const float tn = cam.nearPlane / std::max(dot(dir, normalize(cam.forward)), 1e-3f);
                Hit h;
                ids[((size_t)y * W + x) * 17 + s] = im.rt->intersect(cam.position, dir, tn, INFINITY, kMaskAll, h) ? ((uint64_t)h.instance << 32 | h.triangle) : kSkyIdentity;
            }
    });
    return ids;
}
} // namespace unx::reference
