// Anisotropic GGX (A9, MATERIAL_LAYERS 1.5; owner: render C; shading join: render A). HLSL mirror of MaterialModel.h
// evaluateAnisotropic / anisoFrame / anisoSpecularAlbedo (the C++ model is authoritative; AnisoProbe.hlsl checks the
// mirror) and the per-pixel data the resolve hands to the shading kernels.
//
// Frame (vis-buffer reconstruction, V): the direction is the COOKED tangent (MikkTSpace, interpolated like the normal map's
// TBN), rotated by the material's rotation and orthogonalised against the shading normal. The cooked tangent is chosen
// over a tangent from the uv derivatives because it is continuous across triangles: the uv-derivative tangent is constant
// per triangle, so a brushed highlight on a tessellated curved surface breaks at every edge (Results/C/Aniso/
// tangent_continuity.txt); validation requires tangents on meshes with anisotropic materials.
//
// Band limit (the resolve's rule per axis): the pixel footprint's slope covariance widens each axis,
//   alpha_t'^2 = alpha_t^2 + 2 S_tt,   alpha_b'^2 = alpha_b^2 + 2 S_bb,
// S_tt = ((dn/dx.t)^2 + (dn/dy.t)^2) / 12 for the geometry (the interpolated normal varying linearly over the pixel box)
// plus half the normal map's LEAN variance trace on each axis (mNormalMoments gives the trace). With alpha_t = alpha_b and
// an isotropic covariance this is the isotropic rule alpha'^2 = alpha^2 + trace.
//
// Per-pixel word (R32_UINT, anisotropic pixels only): bits 0..15 the tangent's angle psi in [0, pi) about the shading
// normal AS DECODED from the G-buffer (anisoBasis of octDecode(gbuffer.x), so the shading kernel rebuilds the same basis
// bit for bit; the lobe is symmetric under t -> -t), unorm16 (0.0027 degrees); bits 16..23 and 24..31 the band-limited
// perceptual roughnesses sqrt(alpha_t'), sqrt(alpha_b') as unorm8 (the G-buffer roughness byte's quantisation). The
// G-buffer roughness of an anisotropic pixel is sqrt(sqrt(alpha_t' alpha_b')) (the equal-area isotropic lobe) for readers
// that do not shade the anisotropic lobe (S, R classification).
//
// Table: the anisotropy (A, B) table follows the sheen table in g_coatTable (GpuScene uploads it only when a material is
// anisotropic): ANISO_TABLE, index ((((jb R + jt) P + k) M + i) 2 + {0: A, 1: B}), M = 32 columns at sqrt(n.v),
// P = 13 azimuths over [0, pi/2], R = 16 rows at sqrt(alpha) per axis; quadrilinear (16 corners, 32 loads).
#ifndef UNX_M_ANISO_HLSLI
#define UNX_M_ANISO_HLSLI
#include "Scene.hlsli"
#include "Passes/Common/MaterialModel.hlsli"

#define ANISO_PI 3.14159265358979
#define ANISO_TABLE (MODEL_SHEEN_TABLE + 2u * MODEL_SHEEN_MU * MODEL_SHEEN_R)
#define ANISO_M 32u
#define ANISO_P 13u
#define ANISO_R 16u

struct AnisoRecord
{
    float strength;    // s
    float2 rotation;   // (cos theta, sin theta)
};
AnisoRecord anisoRecordOf(GpuMaterial m)
{
    AnisoRecord a;
    a.strength = 0;
    a.rotation = float2(1, 0);
    if ((m.classFlags & MATERIAL_ANISOTROPIC) != 0)
    {
        const GpuMaterialLayers l = loadMaterialLayers(m.classFlags >> 16);
        a.strength = l.anisotropy;
        a.rotation = l.anisotropyRotation;
    }
    return a;
}

// (alpha_t, alpha_b) = (alpha + (1 - alpha) s^2, alpha), alpha = max(r^2, 1e-4)
float2 anisoAlphas(float roughness, float strength)
{
    const float a = max(roughness * roughness, 1e-4), s = saturate(strength);
    return float2(a + (1 - a) * s * s, a);
}

// Local = (w.t, w.b, w.n).
float anisoD(float3 h, float2 alpha)
{
    const float x = h.x / alpha.x, y = h.y / alpha.y, d = x * x + y * y + h.z * h.z;
    return 1 / (ANISO_PI * alpha.x * alpha.y * d * d);
}
float anisoV(float3 v, float3 l, float2 alpha)
{
    const float gv = l.z * length(float3(alpha.x * v.x, alpha.y * v.y, v.z));
    const float gl = v.z * length(float3(alpha.x * l.x, alpha.y * l.y, l.z));
    return 0.5 / (gv + gl);
}

// (A_a, B_a): the lobe's directional albedo split by the Schlick weight (f0 A + B with Schlick Fresnel).
float2 anisoSpecularAlbedo(float3 v, float2 alpha)
{
    StructuredBuffer<float> t = ResourceDescriptorHeap[g_coatTable];
    const float x = sqrt(saturate(v.z)) * (ANISO_M - 1);
    const float y = atan2(abs(v.y), abs(v.x)) / (0.5 * ANISO_PI) * (ANISO_P - 1);
    const float zt = sqrt(saturate(alpha.x)) * (ANISO_R - 1), zb = sqrt(saturate(alpha.y)) * (ANISO_R - 1);
    const uint x0 = min(uint(x), ANISO_M - 2), y0 = min(uint(y), ANISO_P - 2), t0 = min(uint(zt), ANISO_R - 2), b0 = min(uint(zb), ANISO_R - 2);
    const float fx = x - x0, fy = y - y0, ft = zt - t0, fb = zb - b0;
    float2 sum = 0;
    [unroll] for (uint c = 0; c < 16; ++c)
    {
        const uint dx = c & 1, dy = (c >> 1) & 1, dt = (c >> 2) & 1, db = c >> 3;
        const float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dt ? ft : 1 - ft) * (db ? fb : 1 - fb);
        const uint at = ANISO_TABLE + 2 * ((((b0 + db) * ANISO_R + (t0 + dt)) * ANISO_P + (y0 + dy)) * ANISO_M + (x0 + dx));
        sum += w * float2(t[at], t[at + 1]);
    }
    return sum;
}

// The specular lobe with Schlick Fresnel and the multiple-scattering compensation (MaterialModel.h evaluateAnisotropic
// without the diffuse term): F D V (1 + f0 (1 / E_a(v) - 1)), for n.v, n.l > 0. ab = anisoSpecularAlbedo(v) (per pixel).
float3 anisoSpecular(float3 f0, float3 v, float3 l, float2 alpha, float2 ab)
{
    const float3 h = normalize(v + l);
    const float3 f = f0 + (1 - f0) * pow(1 - saturate(dot(v, h)), 5);
    return f * (anisoD(float3(h.xy, max(h.z, 0)), alpha) * anisoV(v, l, alpha)) * (1 + f0 * (1 / (ab.x + ab.y) - 1));
}

// The frame (MaterialModel.h anisoFrame): T, N the interpolated tangent and normal (any length), sign the bitangent sign,
// rotation (cos, sin), n the unit shading normal. Returns false where it is degenerate (then t, b = anisoBasis(n)).
bool anisoFrame(float3 T, float sign, float3 N, float2 rotation, float3 n, out float3 t, out float3 b)
{
    const float3 Nh = N * rsqrt(max(dot(N, N), 1e-30));
    float3 Tp = T - Nh * dot(Nh, T);
    const float tl = dot(Tp, Tp);
    float3 d = 0;
    if (tl > 1e-24)
    {
        Tp *= rsqrt(tl);
        d = Tp * rotation.x + cross(Nh, Tp) * ((sign < 0 ? -1.0 : 1.0) * rotation.y);
    }
    const float3 dp = d - n * dot(n, d);
    const float dl = dot(dp, dp);
    if (dl > 1e-24)
    {
        t = dp * rsqrt(dl);
        b = cross(n, t);
        return true;
    }
    // (degenerate: any tangent; unreachable for validated meshes except where the rotated tangent is the normal itself)
    const float s = n.z >= 0 ? 1.0 : -1.0, a = -1 / (s + n.z), c = n.x * n.y * a;
    t = float3(1 + s * n.x * n.x * a, s * c, -s * n.x);
    b = cross(n, t);
    return false;
}

// V: the frame of a reconstructed surface (MaterialSurface.hlsli MSurface: the interpolants at the pixel centre) for the
// final shading normal n (normal map, decals and surface layers applied, back side flipped: the caller's n). The
// direction d is built from the unflipped interpolants on both sides of a two-sided surface: it is one line on the surface.
bool anisoFrameOfSurface(float3 tangent, float tangentSign, float3 normal, AnisoRecord a, float3 n, out float3 t, out float3 b)
{
    return anisoFrame(tangent, tangentSign, normal, a.rotation, n, t, b);
}

// Band limit per axis (the header's rule): geometric part from the normal's screen derivatives, the map's trace split.
float2 anisoBandLimit(float2 alpha, float3 t, float3 b, float3 dndx, float3 dndy, float mapVariance)
{
    const float stt = (dot(dndx, t) * dot(dndx, t) + dot(dndy, t) * dot(dndy, t)) / 12.0;
    const float sbb = (dot(dndx, b) * dot(dndx, b) + dot(dndy, b) * dot(dndy, b)) / 12.0;
    return sqrt(alpha * alpha + float2(2 * stt + mapVariance, 2 * sbb + mapVariance));
}

// Branchless orthonormal basis about a unit normal (Duff et al. 2017): the reference the word's angle is measured from.
void anisoBasis(float3 n, out float3 b1, out float3 b2)
{
    const float s = n.z >= 0 ? 1.0 : -1.0, a = -1 / (s + n.z), c = n.x * n.y * a;
    b1 = float3(1 + s * n.x * n.x * a, s * c, -s * n.x);
    b2 = float3(c, s + n.y * n.y * a, -n.y);
}

// Word: t (unit, about the shading normal) measured in the basis of nDecoded = octDecode(the G-buffer's normal word).
uint anisoPackWord(float3 nDecoded, float3 t, float2 alphaFiltered)
{
    float3 b1, b2;
    anisoBasis(nDecoded, b1, b2);
    float psi = atan2(dot(t, b2), dot(t, b1));  // (-pi, pi]
    if (psi < 0) psi += ANISO_PI;               // t and -t are the same lobe
    const uint a = min(uint(round(psi / ANISO_PI * 65536.0)), 65536u) & 0xFFFFu;  // pi wraps to 0
    const uint rt = uint(round(saturate(sqrt(alphaFiltered.x)) * 255.0)), rb = uint(round(saturate(sqrt(alphaFiltered.y)) * 255.0));
    return a | (rt << 16) | (rb << 24);
}

// Unpack for the shading normal n (nDecoded bent towards the viewer, or itself): t, b orthonormal about n, the alphas.
void anisoUnpackWord(uint word, float3 nDecoded, float3 n, out float3 t, out float3 b, out float2 alpha)
{
    float3 b1, b2;
    anisoBasis(nDecoded, b1, b2);
    float s, c;
    sincos((word & 0xFFFFu) * (ANISO_PI / 65536.0), s, c);
    const float3 d = b1 * c + b2 * s;
    t = normalize(d - n * dot(n, d));
    b = cross(n, t);
    const float rt = ((word >> 16) & 0xFFu) / 255.0, rb = (word >> 24) / 255.0;
    alpha = max(float2(rt * rt, rb * rb), 1e-4);
}

// Visible-normal sampling of the stretched lobe (Heitz 2018) for rays (R's exact sampling): the microfacet normal h
// (local) for the unit local view v and u in [0, 1)^2; pdf of the reflected l = G1(v) D(h) / (4 v.n) (anisoPdf).
float3 anisoSampleVndf(float3 v, float2 alpha, float2 u)
{
    const float3 vh = normalize(float3(alpha.x * v.x, alpha.y * v.y, v.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0 ? float3(-vh.y, vh.x, 0) * rsqrt(lensq) : float3(1, 0, 0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u.x), phi = 2 * ANISO_PI * u.y;
    const float p1 = r * cos(phi), sb = 0.5 * (1 + vh.z);
    const float p2 = (1 - sb) * sqrt(max(0, 1 - p1 * p1)) + sb * r * sin(phi);
    const float3 nh = t1 * p1 + t2 * p2 + vh * sqrt(max(0, 1 - p1 * p1 - p2 * p2));
    return normalize(float3(alpha.x * nh.x, alpha.y * nh.y, max(0, nh.z)));
}
float anisoPdf(float3 v, float3 l, float2 alpha)
{
    const float3 h = normalize(v + l);
    const float lv = length(float3(alpha.x * v.x, alpha.y * v.y, v.z));
    const float g1 = 2 * v.z / (v.z + lv);
    return g1 * anisoD(float3(h.xy, max(h.z, 0)), alpha) / (4 * v.z);
}

#endif
