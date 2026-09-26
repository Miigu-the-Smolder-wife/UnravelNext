// unx-kernel: cs_6_6 main
// The tau_top table on the GPU (photo-mode start: the CPU build took 22 s [measured, CPU contended]): one thread per
// entry, AtmosphereModel::tableEntry (Reference/PathTracer/src/Atmosphere.cpp) with the same operations in the same
// order - double geometry and accumulation, float coefficients, 256 Gauss-Legendre panels split at the closest approach.
// Only base double operations (add, mul, min / max, compare, float <-> double; no double division, sqrt, exp or
// integer <-> double conversion, the "double extensions" D3D12 does not guarantee; integers pass through float, exact
// below 2^24): division is a float reciprocal refined by Newton steps plus one residual correction, sqrt a float rsqrt
// refined by Newton steps plus one correction, exp a Cody-Waite reduction and a degree-13 polynomial - each within about
// one double ulp of the CPU's, far below the float the coefficients and the table are stored in. 'precise' keeps the
// float and double sums unfused, as the CPU computes them (MSVC /fp:precise, no contraction).
// Dispatch bound: count entries per dispatch (the host sends at most 16384), 2048 samples each.
// Root: x0 = output (RWByteAddressBuffer, 12 bytes per entry, index = ir * 2048 + im), y0 = first entry, w = count,
// h / sampleBegin = H = sqrt(Rt^2 - R^2) (double bits from the CPU), sampleEnd = parameters (ByteAddressBuffer, 20
// floats: rayleigh / mie scale height, ozone centre / width, rayleigh scattering rgb, R, mie scattering rgb, Rt,
// mie absorption rgb, -, ozone absorption rgb, -), pathCount = the output's first entry (0 for the whole table; a check
// of some entries writes them from offset 0).
#include "Common.hlsli"

static const uint kTableMu = 2048, kTableR = 1024, kPanels = 256;
static const double kGlX[8] = { -0.9602898564975363L, -0.7966664774136267L, -0.5255324099163290L, -0.1834346424956498L,
                                0.1834346424956498L, 0.5255324099163290L, 0.7966664774136267L, 0.9602898564975363L };
static const double kGlW[8] = { 0.1012285362903763L, 0.2223810344533745L, 0.3137066458778873L, 0.3626837833783620L,
                                0.3626837833783620L, 0.3137066458778873L, 0.2223810344533745L, 0.1012285362903763L };

double dOf(uint i) { return (double)(float)i; }  // exact for i < 2^24
double dDiv(double a, double b)
{
    precise double q = (double)(1.0f / (float)b);
    q = q * (2.0L - b * q);
    q = q * (2.0L - b * q);
    precise double y = a * q;
    return y + (a - b * y) * q;
}
double dSqrt(double x)
{
    if (!(x > 0.0L)) return 0.0L;
    precise double y = (double)rsqrt((float)x);
    y = y * (1.5L - 0.5L * x * y * y);
    y = y * (1.5L - 0.5L * x * y * y);
    y = y * (1.5L - 0.5L * x * y * y);
    precise double s = x * y;
    return s + 0.5L * y * (x - s * s);
}
// exp(x) for x <= 0 (the scale-height exponents); results below e^-708 are 0 (far below float's range)
double dExp(double x)
{
    if (x < -708.0L) return 0.0L;
    // k: nearest integer to x / ln 2 (in float: |r| may exceed ln2 / 2 by float rounding, which the polynomial covers)
    const int k = (int)((float)(x * 1.4426950408889634L) - 0.5f);
    const double kd = (double)(float)k;
    precise double r = (x - kd * 6.93147180369123816490e-01L) - kd * 1.90821492927058770002e-10L;  // fdlibm ln2 hi / lo
    precise double p = 1.6059043836821613e-10L;  // 1 / 13!
    p = p * r + 2.0876756987868099e-09L;
    p = p * r + 2.5052108385441720e-08L;
    p = p * r + 2.7557319223985893e-07L;
    p = p * r + 2.7557319223985888e-06L;
    p = p * r + 2.4801587301587302e-05L;
    p = p * r + 1.9841269841269841e-04L;
    p = p * r + 1.3888888888888889e-03L;
    p = p * r + 8.3333333333333332e-03L;
    p = p * r + 4.1666666666666664e-02L;
    p = p * r + 1.6666666666666666e-01L;
    p = p * r + 0.5L;
    p = p * r + 1.0L;
    p = p * r + 1.0L;
    return p * asdouble(0u, (uint)(k + 1023) << 20);
}

struct AtmParams
{
    float rayleighH, mieH, ozoneCenter, ozoneWidth;
    float3 rayleigh; double R;
    float3 mie; double Rt;
    float3 mieAbsorption, ozoneAbsorption;
};

// AtmosphereModel::at(h).extinction
float3 extinctionAt(AtmParams a, double h)
{
    precise float rayleigh = (float)dExp(dDiv(-h, (double)a.rayleighH));
    precise float mie = (float)dExp(dDiv(-h, (double)a.mieH));
    precise double oz = 1.0L - dDiv(abs(h - (double)a.ozoneCenter), (double)a.ozoneWidth);
    precise float ozone = (float)max(0.0L, oz);
    precise float3 sR = a.rayleigh * rayleigh;
    precise float3 sM = a.mie * mie;
    precise float3 e = sR + sM;
    e = e + a.mieAbsorption * mie;
    e = e + a.ozoneAbsorption * ozone;
    return e;
}

// AtmosphereModel::integrate(o, d, t0, t1, panels), o = (0, oy, 0), d = (dx, dy, 0) in float
float3 integrate(AtmParams a, double oy, float dx, float dy, double t0, double t1, uint panels)
{
    precise double acc0 = 0, acc1 = 0, acc2 = 0;
    precise double w = dDiv(t1 - t0, dOf(panels));
    for (uint p = 0; p < panels; ++p)
    {
        precise double mid = t0 + (dOf(p) + 0.5L) * w, half = 0.5L * w;
        [unroll] for (uint k = 0; k < 8; ++k)
        {
            precise double t = mid + half * kGlX[k];
            precise double px = 0.0L + (double)dx * t, py = oy + (double)dy * t;
            precise double y = py + a.R;
            precise double h = dSqrt(px * px + y * y + 0.0L * 0.0L) - a.R;
            const float3 e = extinctionAt(a, h);
            precise double wk = kGlW[k] * half;
            acc0 += wk * (double)e.x;
            acc1 += wk * (double)e.y;
            acc2 += wk * (double)e.z;
        }
    }
    return float3((float)acc0, (float)acc1, (float)acc2);
}

// AtmosphereModel::depthTopDirect(r, mu, 256)
float3 depthTopDirect(AtmParams a, double r, double mu)
{
    const double oy = r - a.R;
    const float dx = (float)dSqrt(max(0.0L, 1.0L - mu * mu)), dy = (float)mu;
    precise double len = -r * mu + dSqrt(max(0.0L, r * r * (mu * mu - 1.0L) + a.Rt * a.Rt));
    if (len <= 0.0L) return float3(0, 0, 0);
    precise double tStar = -r * mu;
    if (tStar > 0.0L && tStar < len)
    {
        // lround(panels tStar / len) = floor(x + 0.5): a float estimate, corrected by exact double comparisons
        precise double x = dDiv(dOf(kPanels) * tStar, len) + 0.5L;
        uint f = (uint)(float)x;
        if (dOf(f) > x) --f;
        else if (dOf(f + 1u) <= x) ++f;
        const uint p0 = max(1u, f);
        precise float3 s = integrate(a, oy, dx, dy, 0.0L, tStar, p0) + integrate(a, oy, dx, dy, tStar, len, max(1u, kPanels - p0));
        return s;
    }
    return integrate(a, oy, dx, dy, 0.0L, len, kPanels);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_root.w) return;
    const uint entry = g_root.y0 + id.x;
    const uint ir = entry / kTableMu, im = entry % kTableMu;
    ByteAddressBuffer pb = ResourceDescriptorHeap[g_root.sampleEnd];
    AtmParams a;
    a.rayleighH = pb.Load<float>(0);
    a.mieH = pb.Load<float>(4);
    a.ozoneCenter = pb.Load<float>(8);
    a.ozoneWidth = pb.Load<float>(12);
    a.rayleigh = pb.Load<float3>(16);
    a.R = (double)pb.Load<float>(28);
    a.mie = pb.Load<float3>(32);
    a.Rt = (double)pb.Load<float>(44);
    a.mieAbsorption = pb.Load<float3>(48);
    a.ozoneAbsorption = pb.Load<float3>(64);
    const double H = asdouble(g_root.h, g_root.sampleBegin);
    // AtmosphereModel::tableEntry
    precise double xr = dDiv(dOf(ir), dOf(kTableR - 1));
    precise double rho = H * xr;
    precise double r = dSqrt(rho * rho + a.R * a.R);
    precise double dMin = a.Rt - r, dMax = rho + H;
    precise double xm = dDiv(dOf(im), dOf(kTableMu - 1));
    precise double d = dMin + xm * (dMax - dMin);
    precise double mu = 1.0L;
    if (d > 0.0L) mu = clamp(dDiv(H * H - rho * rho - d * d, 2.0L * r * d), -1.0L, 1.0L);
    const float3 t = depthTopDirect(a, r, mu);
    RWByteAddressBuffer o = ResourceDescriptorHeap[g_root.x0];
    o.Store3((entry - g_root.pathCount) * 12, asuint(t));
}
