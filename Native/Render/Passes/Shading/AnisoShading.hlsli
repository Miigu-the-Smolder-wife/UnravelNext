// A9 anisotropic GGX in the shading kernels (MATERIAL_LAYERS 1.5; the lobe, table and word are render C's Aniso.hlsli;
// this file is render A's join). A pixel of an anisotropic material carries the resolve's word: the tangent angle about
// the G-buffer normal and the band-limited (alpha_t', alpha_b'). Its base specular replaces the isotropic lobe:
//   point lights  the lobe itself (exact);
//   sun           shAnisoSunSpecular: one rule for every (alpha_t, alpha_b), narrow, wide or a streak (one axis each);
//   area lights   shAnisoLtc: the stretch that makes the lobe's footprint isotropic, then the isotropic LTC of
//                 alpha_i = sqrt(alpha_t alpha_b) (FEATURES_GAME 14; its error is measured against the lobe's quadrature);
//   indirect      the lobe's albedo f0 A_a + B_a times the radiance of the equal-area isotropic cone (the G-buffer
//                 roughness sqrt(sqrt(alpha_t' alpha_b')) that R and the probes read).
#ifndef UNX_M_ANISO_SHADING_HLSLI
#define UNX_M_ANISO_SHADING_HLSLI
#include "Passes/Material/Aniso.hlsli"
#include "Passes/Shading/AreaLight.hlsli"

struct ShAniso
{
    bool on;
    float3 t, b;     // world tangent and bitangent about the shading normal
    float2 alpha;    // (alpha_t', alpha_b')
    float2 ab;       // (A_a, B_a) of this view direction
};

// The pixel's lobe (on = false for isotropic pixels or scenes without the word texture). nDecoded: the G-buffer normal as
// decoded, n: the shading normal (nDecoded bent towards the viewer).
ShAniso shAnisoOf(uint wordSrv, uint2 pixel, GpuMaterial m, float3 nDecoded, float3 n, float3 v)
{
    ShAniso a = (ShAniso)0;
    a.on = wordSrv != UNX_NONE && (m.classFlags & MATERIAL_ANISOTROPIC) != 0;
    if (!a.on) return a;
    Texture2D<uint> words = ResourceDescriptorHeap[wordSrv];
    anisoUnpackWord(words[pixel], nDecoded, n, a.t, a.b, a.alpha);
    a.ab = anisoSpecularAlbedo(float3(dot(v, a.t), dot(v, a.b), dot(v, n)), a.alpha);
    return a;
}

float3 shAnisoAlbedo(ShAniso a, float3 f0) { return f0 * a.ab.x + a.ab.y; }

// f_s (with Fresnel and the compensation) for n.v, n.l > 0.
float3 shAnisoSpecular(ShAniso a, float3 f0, float3 n, float3 v, float3 l)
{
    const float3x3 L = float3x3(a.t, a.b, n);
    return anisoSpecular(f0, mul(L, v), mul(L, l), a.alpha, a.ab);
}

// L_sun int_cap f_s cos dw. With f_s cos dw_l = F D G2 (v.h) / n.v dw_h and D (h.n) dw_h = P22(m) d^2m (m the slope of h),
// the integral is F G2 (v.h) / (n.v h.n) times the P22 mass of the cap's image in slope space. The prefactor moves by
// < 0.3 deg over the disk and is taken at its centre. In u = m / alpha the slope density is the unit isotropic P22 with
// radial CDF F(rho) = rho^2 / (1 + rho^2); the map l -> u is linearised at the disk centre (its change over the 0.27 deg
// disk is second order), so the cap is an ellipse u0 + theta_s (J0 cos + J1 sin) wherever the lobe is - narrow, wide,
// or narrow across and wide along (a brushed streak). Its mass by Green's theorem, (1 / 2 pi) closed int F(|u|) dphi =
// (1 / N) sum (u x u') / (1 + |u|^2): smooth and periodic in the parameter, so the N-point trapezoid rule converges
// spectrally. The pixel's angular footprint widens each alpha in quadrature (p / 4, shSunLobeFraction's rule). Terminator
// band (the disk across the horizon): the per-point cap quadrature with the clipped cosine, as for the isotropic lobe.
float3 shAnisoSunSpecular(ShAniso a, float3 f0, float3 n, float3 v, float NoV, float3 l0, float3 E, float pixelAngle)
{
    const float thetaS = g_sunAngularRadius, sinS = sin(thetaS), cosS = cos(thetaS);
    const float NoL0 = dot(n, l0);
    if (NoL0 <= -sinS) return 0;
    const float3 dt = normalize(abs(l0.y) < 0.99 ? cross(float3(0, 1, 0), l0) : cross(float3(1, 0, 0), l0));
    const float3 db = cross(l0, dt);
    if (NoL0 < 2 * sinS)
    {
        const float4 glNodes = float4(0.0694318442, 0.3300094782, 0.6699905218, 0.9305681558);
        const float4 glWeights = float4(0.1739274226, 0.3260725774, 0.3260725774, 0.1739274226);
        float3 sum = 0;
        [loop] for (uint i = 0; i < 4; ++i)
        {
            const float c = 1 - glNodes[i] * (1 - cosS), s = sqrt(max(1 - c * c, 0.0));
            [loop] for (uint k = 0; k < 12; ++k)
            {
                float sp, cp;
                sincos((k + 0.5) * (2 * SH_PI / 12), sp, cp);
                const float3 l = l0 * c + (dt * cp + db * sp) * s;
                const float NoL = dot(n, l);
                if (NoL > 0) sum += shAnisoSpecular(a, f0, n, v, l) * (NoL * glWeights[i] / 12);
            }
        }
        return sum * (E * (2 / (1 + cosS)));
    }
    const float3x3 L = float3x3(a.t, a.b, n);
    const float2 alpha = sqrt(a.alpha * a.alpha + pixelAngle * pixelAngle / 16);
    const float3 hp = v + l0;
    const float hl = length(hp);
    const float3 h = hp / hl, hL = mul(L, h);
    const float2 u0 = -hL.xy / (hL.z * alpha);
    float2 J[2];
    [unroll] for (uint j = 0; j < 2; ++j)
    {
        const float3 d = j == 0 ? dt : db;
        const float3 dhL = mul(L, (d - h * dot(h, d)) / hl);
        J[j] = -(dhL.xy - hL.xy * (dhL.z / hL.z)) / (hL.z * alpha);
    }
    float mass = 0;
    [loop] for (uint k = 0; k < 32; ++k)
    {
        float sp, cp;
        sincos((k + 0.5) * (2 * SH_PI / 32), sp, cp);
        const float2 u = u0 + thetaS * (J[0] * cp + J[1] * sp), du = thetaS * (J[1] * cp - J[0] * sp);
        mass += (u.x * du.y - u.y * du.x) / (1 + dot(u, u));
    }
    mass = abs(mass) / 32;
    const float3 vL = mul(L, v), lL = mul(L, l0);
    const float G2 = anisoV(vL, lL, a.alpha) * 4 * NoV * NoL0;
    const float3 F = f0 + (1 - f0) * pow(1 - saturate(dot(v, h)), 5);
    const float3 comp = 1 + f0 * (1 / (a.ab.x + a.ab.y) - 1);
    return F * comp * (G2 * dot(v, h) / (NoV * hL.z) * mass) * (E / (SH_PI * sinS * sinS));
}

// The area-light transform of the anisotropic lobe: world -> the lobe's frame (t, b, n), stretched by
// diag(alpha_i / alpha_t, alpha_i / alpha_b, 1) so the lobe's footprint is isotropic at alpha_i = sqrt(alpha_t alpha_b),
// then the isotropic LTC inverse of alpha_i at the stretched view's n.v in its own shading frame. The integral's magnitude
// is the anisotropic albedo (shAnisoAlbedo).
float3x3 shAnisoLtc(ShAniso a, uint ltcTable, float3 n, float3 v)
{
    const float ai = sqrt(a.alpha.x * a.alpha.y);
    const float3x3 S = float3x3(a.t * (ai / a.alpha.x), a.b * (ai / a.alpha.y), n);  // world -> stretched lobe frame
    const float3 vs = normalize(mul(S, v));
    const float3x3 frame = shShadingFrame(float3(0, 0, 1), vs, vs.z);
    return mul(shLtcInverse(ltcTable, max(vs.z, 1e-4), sqrt(ai)), mul(frame, S));
}

#endif
