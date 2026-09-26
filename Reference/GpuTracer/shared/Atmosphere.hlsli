// The atmosphere of INTERFACES_KO.md 8.3 as a participating medium, the shared form of
// Reference/PathTracer/src/Atmosphere.{h,cpp} (same coefficients, phases, phase sampling, tau_top table lookup,
// Gauss-Legendre integration, segment pdf). The CPU estimator holds positions relative to the planet centre in double;
// here every such quantity is rewritten without cancellation so float keeps its full relative precision:
//   q = r^2 - R^2 = |p|^2 + 2 R p.y          (p relative to the scene origin, which sits on the surface)
//   h = r - R = q / (r + R)                   altitude
//   rho = sqrt(q),  mu_horizon = -rho / r,  d_top = (H^2 - q + (r mu)^2)^(1/2) - r mu  (conjugate form when r mu > 0)
// and the ray-sphere quadratics (planet, top of the atmosphere) are formed in double (IEEE add/mul, the same operations
// as the CPU estimator) and solved with the conjugate root, so their tangent cases keep the CPU's precision. The table
// is the CPU estimator's (AtmosphereModel, 2048 x 1024, built in double) read through rtAtmTableFetch(ir, im).
#ifndef UNX_RT_ATMOSPHERE_HLSLI
#define UNX_RT_ATMOSPHERE_HLSLI
#include "Compat.hlsli"

RT_BEGIN_NAMESPACE
RT_CONST float kRtShortSegment = 20000.0f;
RT_CONST uint kRtAtmTableMu = 2048;
RT_CONST uint kRtAtmTableR = 1024;

struct RtAtmosphere
{
    float R;          // bottom radius
    float Rt;         // top radius
    float H2;         // Rt^2 - R^2
    float H;          // sqrt(H2)
    float rayleighScaleHeight;
    float mieScaleHeight;
    float mieG;
    float ozoneCenter;
    float ozoneWidth;
    float3 rayleighScattering;
    float3 mieScattering;
    float3 mieAbsorption;
    float3 ozoneAbsorption;
    float3 groundAlbedo;
};

struct RtAtmCoefficients
{
    float3 scatteringRayleigh;
    float3 scatteringMie;
    float3 extinction;
};

float3 rtAtmTableFetch(uint ir, uint im);  // defined by the includer: tau_top[ir * 2048 + im]

RT_INLINE RtAtmCoefficients rtAtmAt(RtAtmosphere a, float h)
{
    const float rayleigh = exp(-h / a.rayleighScaleHeight), mie = exp(-h / a.mieScaleHeight);
    const float ozone = max(0.0f, 1.0f - abs(h - a.ozoneCenter) / a.ozoneWidth);
    RtAtmCoefficients c;
    c.scatteringRayleigh = a.rayleighScattering * rayleigh;
    c.scatteringMie = a.mieScattering * mie;
    c.extinction = c.scatteringRayleigh + c.scatteringMie + a.mieAbsorption * mie + a.ozoneAbsorption * ozone;
    return c;
}

// r^2 - R^2 for a scene-relative point.
RT_INLINE float rtAtmQ(RtAtmosphere a, float3 p) { return dot(p, p) + 2.0f * a.R * p.y; }
RT_INLINE float rtAtmRadius(RtAtmosphere a, float q) { return sqrt(a.R * a.R + q); }
RT_INLINE float rtAtmAltitude(RtAtmosphere a, float3 p)
{
    const float q = rtAtmQ(a, p);
    return q / (rtAtmRadius(a, q) + a.R);
}
// (p - centre) . d for the planet centre (0, -R, 0).
RT_INLINE float rtAtmRadialDot(RtAtmosphere a, float3 p, float3 d) { return dot(p, d) + a.R * d.y; }

RT_INLINE float rtPhaseRayleigh(float c) { return 0.05968310365946075f * (1 + c * c); }
RT_INLINE float rtPhaseMie(RtAtmosphere a, float c)
{
    const float g = a.mieG, den = 1 + g * g - 2 * g * c;
    return (1 - g * g) / (12.566370614359172f * den * sqrt(den));
}
RT_INLINE float rtCbrt(float x) { return x < 0 ? -pow(-x, 1.0f / 3.0f) : pow(x, 1.0f / 3.0f); }

// Direction for scattering at a point reached along 'forward', from the Rayleigh/Mie mixture weighted by wR, wM; pdf
// equals the mixture phase value.
RT_INLINE float3 rtSamplePhase(RtAtmosphere a, float3 forward, float wR, float wM, float u1, float u2, float u3, RT_OUT(float) pdf)
{
    float c;
    if (u1 * (wR + wM) < wR)
    {
        // Rayleigh: mu^3 + 3 mu + (4 - 8u) = 0 (Cardano).
        const float q = 4.0f - 8.0f * u2, s = sqrt(q * q / 4.0f + 1.0f);
        c = rtCbrt(-q / 2.0f + s) + rtCbrt(-q / 2.0f - s);
    }
    else
    {
        const float g = a.mieG;
        const float t = (1 - g * g) / (1 - g + 2 * g * u2);
        c = (1 + g * g - t * t) / (2 * g);
    }
    c = clamp(c, -1.0f, 1.0f);
    const float sinT = sqrt(max(0.0f, 1 - c * c)), phi = 6.283185307f * u3;
    float3 t1, t2;
    rtOrthonormal(forward, t1, t2);
    const float3 w = normalize(forward * c + t1 * (sinT * cos(phi)) + t2 * (sinT * sin(phi)));
    pdf = (wR * rtPhaseRayleigh(c) + wM * rtPhaseMie(a, c)) / (wR + wM);
    return w;
}

// Distance to the planet (ground) along d; negative if not hit. Quadratic coefficients in double as on the CPU.
RT_INLINE float rtAtmGroundDistance(RtAtmosphere a, float3 o, float3 d)
{
    const double R = (double)a.R;
    const double ox = (double)o.x, oy = (double)o.y + R, oz = (double)o.z;
    const double b = ox * (double)d.x + oy * (double)d.y + oz * (double)d.z;
    const double c = ox * ox + oy * oy + oz * oz - R * R;
    if (c <= 0) return -1;
    const double disc = b * b - c;
    if (disc < 0 || b > 0) return -1;
    // Conjugate root: t = c / (-b + sqrt(disc)) (the CPU's -b - sqrt(disc) cancels when c is small).
    const float t = (float)c / ((float)(-b) + sqrt((float)disc));
    return t > 0 ? t : -1;
}

RT_INLINE float rtAtmTopDistance(RtAtmosphere a, float3 o, float3 d)
{
    const double R = (double)a.R, Rt = (double)a.Rt;
    const double ox = (double)o.x, oy = (double)o.y + R, oz = (double)o.z;
    const double b = ox * (double)d.x + oy * (double)d.y + oz * (double)d.z;
    const double c = ox * ox + oy * oy + oz * oz - Rt * Rt;
    const double disc = b * b - c;
    if (disc < 0) return -1;
    const float sd = sqrt((float)disc);
    if (b > 0) return (float)(-c) / ((float)b + sd);
    return (float)(-b) + sd;
}

RT_CONST float kRtGlX[8] = { -0.9602898564975363f, -0.7966664774136267f, -0.5255324099163290f, -0.1834346424956498f,
                             0.1834346424956498f, 0.5255324099163290f, 0.7966664774136267f, 0.9602898564975363f };
RT_CONST float kRtGlW[8] = { 0.1012285362903763f, 0.2223810344533745f, 0.3137066458778873f, 0.3626837833783620f,
                             0.3626837833783620f, 0.3137066458778873f, 0.2223810344533745f, 0.1012285362903763f };

RT_INLINE float3 rtAtmIntegrate(RtAtmosphere a, float3 o, float3 d, float t0, float t1, uint panels)
{
    float3 acc = rtSplat3(0);
    const float w = (t1 - t0) / (float)panels;
    const float half = 0.5f * w;
    RT_LOOP for (uint n = 0; n < panels * 8; ++n)
    {
        const uint p = n >> 3, k = n & 7;
        const float mid = t0 + ((float)p + 0.5f) * w;
        const float t = mid + half * kRtGlX[k];
        const RtAtmCoefficients c = rtAtmAt(a, rtAtmAltitude(a, o + d * t));
        acc += c.extinction * (kRtGlW[k] * half);
    }
    return acc;
}

RT_INLINE float rtCubicWeight(float t, int k)
{
    const float t2 = t * t, t3 = t2 * t;
    if (k == 0) return -0.5f * t3 + t2 - 0.5f * t;
    if (k == 1) return 1.5f * t3 - 2.5f * t2 + 1.0f;
    if (k == 2) return -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    return 0.5f * t3 - 0.5f * t2;
}

// Table sample with the CPU's quadratic extrapolation outside the grid (the Catmull-Rom stencil of boundary cells).
RT_INLINE float3 rtAtmTableRow(int r, int m)
{
    const int nm = (int)kRtAtmTableMu;
    if (m < 0) return rtAtmTableFetch((uint)r, 0u) * 3.0f - rtAtmTableFetch((uint)r, 1u) * 3.0f + rtAtmTableFetch((uint)r, 2u);
    if (m >= nm) return rtAtmTableFetch((uint)r, (uint)(nm - 1)) * 3.0f - rtAtmTableFetch((uint)r, (uint)(nm - 2)) * 3.0f + rtAtmTableFetch((uint)r, (uint)(nm - 3));
    return rtAtmTableFetch((uint)r, (uint)m);
}
RT_INLINE float3 rtAtmTableAt(int rr, int mm)
{
    const int nr = (int)kRtAtmTableR;
    if (rr < 0) return rtAtmTableRow(0, mm) * 3.0f - rtAtmTableRow(1, mm) * 3.0f + rtAtmTableRow(2, mm);
    if (rr >= nr) return rtAtmTableRow(nr - 1, mm) * 3.0f - rtAtmTableRow(nr - 2, mm) * 3.0f + rtAtmTableRow(nr - 3, mm);
    return rtAtmTableRow(rr, mm);
}

// tau to the top of the atmosphere for altitude h (clamped to [0, Rt - R]) and direction cosine mu (bicubic, Bruneton
// parameterisation x_mu = (d - d_min) / (d_max - d_min), x_r = rho / H).
RT_INLINE float3 rtAtmDepthTopTable(RtAtmosphere a, float h, float mu)
{
    h = clamp(h, 0.0f, a.Rt - a.R);
    const float r = a.R + h;
    const float q = h * (2.0f * a.R + h);  // r^2 - R^2
    const float rho = sqrt(max(0.0f, q));
    const float rmu = r * mu;
    const float disc = max(0.0f, (a.H2 - q) + rmu * rmu);
    const float sd = sqrt(disc);
    const float d = rmu > 0 ? (a.H2 - q) / (rmu + sd) : sd - rmu;
    const float dMin = (a.Rt - a.R) - h, dMax = rho + a.H;
    const float xm = clamp((d - dMin) / max(dMax - dMin, 1e-9f), 0.0f, 1.0f), xr = rho / a.H;
    const float fm = xm * (float)(kRtAtmTableMu - 1), fr = xr * (float)(kRtAtmTableR - 1);
    const int im = min((int)fm, (int)kRtAtmTableMu - 2), ir = min((int)fr, (int)kRtAtmTableR - 2);
    const float tm = fm - (float)im, tr = fr - (float)ir;
    // 4 x 4 Catmull-Rom stencil; boundary cells take the extrapolated samples.
    const bool inside = ir >= 1 && ir + 2 < (int)kRtAtmTableR && im >= 1 && im + 2 < (int)kRtAtmTableMu;
    float3 acc = rtSplat3(0);
    RT_LOOP for (int k = 0; k < 16; ++k)
    {
        const int ka = k >> 2, kb = k & 3;
        const float3 v = inside ? rtAtmTableFetch((uint)(ir - 1 + ka), (uint)(im - 1 + kb)) : rtAtmTableAt(ir - 1 + ka, im - 1 + kb);
        acc += v * (rtCubicWeight(tm, kb) * rtCubicWeight(tr, ka));
    }
    return rtMax3v(acc, rtSplat3(0));
}

RT_INLINE float3 rtAtmInfinity() { return rtSplat3(rtAsFloat(0x7F800000u)); }

// Optical depth from o to the top of the atmosphere along d; +infinity if the ray hits the ground.
RT_INLINE float3 rtAtmDepthToTop(RtAtmosphere a, float3 o, float3 d)
{
    const float q = rtAtmQ(a, o);
    const float r = rtAtmRadius(a, q);
    const float b = rtAtmRadialDot(a, o, d);
    if (q < 0)
    {
        // Below the planet surface (a scene valley): integrate up to the surface crossing, then the table at h = 0.
        const float disc = b * b - q;
        const float sd = sqrt(max(disc, 0.0f));
        const float tExit = b > 0 ? (-q) / (b + sd) : sd - b;
        const float3 e = o + d * tExit;
        const float qe = rtAtmQ(a, e);
        const float mue = rtAtmRadialDot(a, e, d) / rtAtmRadius(a, qe);
        const uint panels = min(max(1u, (uint)ceil(tExit / 2000.0f)), 4096u);
        return rtAtmIntegrate(a, o, d, 0, tExit, panels) + rtAtmDepthTopTable(a, 0.0f, max(mue, 0.0f));
    }
    const float h = q / (r + a.R);
    if (h > a.Rt - a.R) return rtSplat3(0);
    // Ground intersection: mu < mu_horizon = -rho / r  <=>  b < -rho.
    if (b < -sqrt(q)) return rtAtmInfinity();
    return rtAtmDepthTopTable(a, h, b / r);
}

// Optical depth of the segment o + d t, t in [0, len] (the segment must not pass through the planet).
RT_INLINE float3 rtAtmOpticalDepth(RtAtmosphere a, float3 o, float3 d, float len)
{
    if (len <= 0) return rtSplat3(0);
    if (len < kRtShortSegment)
    {
        // One 8-point panel per 2 km of altitude span (the CPU's rule).
        const float3 e = o + d * len;
        const float ho = rtAtmAltitude(a, o), he = rtAtmAltitude(a, e);
        float hMin = min(ho, he);
        const float hMax = max(ho, he);
        const float tStar = -rtAtmRadialDot(a, o, d);
        if (tStar > 0 && tStar < len) hMin = min(hMin, rtAtmAltitude(a, o + d * tStar));
        const uint panels = max(1u, (uint)ceil((hMax - hMin) / 2000.0f));
        return rtAtmIntegrate(a, o, d, 0, len, panels);
    }
    // Difference of two depths to the top: forward from o and e when the ray misses the ground, else backward from e
    // and o (one evaluation site for both).
    const float3 e = o + d * len;
    const bool forward = rtAtmGroundDistance(a, o, d) < 0;
    float3 tau[2];
    RT_LOOP for (uint k = 0; k < 2; ++k)
    {
        const float3 p = (k == 0) == forward ? o : e;
        tau[k] = rtAtmDepthToTop(a, p, forward ? d : -d);
    }
    return rtMax3v(tau[0] - tau[1], rtSplat3(0));
}

// Pdf for points on a segment [0, len] (PathTracer.cpp SegmentPdf): uniform for short segments; for long ones a
// mixture of two truncated exponentials (one Rayleigh / Mie scale height of climb, capped by the curvature distance)
// and 10 % uniform.
struct RtSegmentPdf
{
    bool uniformPdf;
    float len;
    float scale0;
    float scale1;
    float norm0;  // 1 - exp(-len / scale)
    float norm1;
};
RT_CONST float kRtSegW0 = 0.45f;
RT_CONST float kRtSegW1 = 0.45f;
RT_CONST float kRtSegW2 = 0.10f;

RT_INLINE RtSegmentPdf rtSegmentPdf(RtAtmosphere a, float3 o, float3 d, float len)
{
    RtSegmentPdf p;
    p.len = len;
    p.uniformPdf = true;
    p.scale0 = p.scale1 = p.norm0 = p.norm1 = 0;
    if (len < kRtShortSegment) return p;
    p.uniformPdf = false;
    const float q = rtAtmQ(a, o);
    const float mu = rtAtmRadialDot(a, o, d) / rtAtmRadius(a, q);
    const float c0 = sqrt(2.0f * a.R * a.rayleighScaleHeight), c1 = sqrt(2.0f * a.R * a.mieScaleHeight);
    p.scale0 = mu > 1e-9f ? min(a.rayleighScaleHeight / mu, c0) : c0;
    p.scale1 = mu > 1e-9f ? min(a.mieScaleHeight / mu, c1) : c1;
    p.norm0 = -rtExpm1(-len / p.scale0);
    p.norm1 = -rtExpm1(-len / p.scale1);
    return p;
}
RT_INLINE float rtSegmentPdfAt(RtSegmentPdf p, float t)
{
    if (p.uniformPdf) return 1.0f / p.len;
    return kRtSegW2 / p.len + kRtSegW0 * exp(-t / p.scale0) / (p.scale0 * p.norm0) + kRtSegW1 * exp(-t / p.scale1) / (p.scale1 * p.norm1);
}
RT_INLINE float rtSegmentSample(RtSegmentPdf p, float u, RT_OUT(float) pdfOut)
{
    float t;
    if (p.uniformPdf) t = u * p.len;
    else if (u < kRtSegW0 + kRtSegW1)
    {
        const bool first = u < kRtSegW0;
        const float v = first ? u / kRtSegW0 : (u - kRtSegW0) / kRtSegW1;
        const float sc = first ? p.scale0 : p.scale1, nm = first ? p.norm0 : p.norm1;
        t = -sc * rtLog1p(-min(v, 0.99999994f) * nm);
    }
    else t = (u - kRtSegW0 - kRtSegW1) / kRtSegW2 * p.len;
    t = clamp(t, 0.0f, p.len);
    pdfOut = rtSegmentPdfAt(p, t);
    return t;
}
RT_END_NAMESPACE

#endif
