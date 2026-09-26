// unx-kernel: cs_6_6 main
// Water foam F (Foam.hlsli), once per sea state: the automatic breaking threshold J_t whose mean foam equals the target
// W (observed whitecap coverage, Monahan & O'Muircheartaigh 1980: W = 3.84e-6 U^3.41, U the 10 m wind in m/s).
// Linear waves make the displacement gradient (a, b, c) = (dDx/dx, dDz/dz, dDx/dz) a zero-mean Gaussian with covariance
// Sigma, and its time derivative a Gaussian independent of it (each mode's value and rate are cos and sin of one phase)
// with covariance Sigma' (FoamVariance). The Jacobian J = (1 + a)(1 + b) - c^2 is not Gaussian: with u = a + b,
// v = a - b, w = c it is J = ((2 + u)^2 - v^2 - 4 w^2) / 4, so for a fixed (v, w) the event J < t is |2 + u| < r,
// r = sqrt(4 t + v^2 + 4 w^2), and J = t has the two roots u = -2 +- r with |dJ/du| = r / 2. Hence, exactly,
//   breaking fraction  b(t) = E_(v,w)[ Phi((r - 2 - mu) / s) - Phi((-r - 2 - mu) / s) ]
//   breaking onsets    lambda(t) = 1/2 E_(v,w)[ sum over roots phi((u - mu) / s) / s * sqrt(2 / pi) sd(u, v, w) / (r / 2) ]
//                      (Rice: downcrossings of t per second; sd^2 = g' Sigma' g, g = dJ/d(a, b, c) = (1 + b, 1 + a, -2c))
// with mu, s the mean and deviation of u given (v, w). E_(v,w) is a 32 x 32 Gauss-Hermite product rule over (v, w)
// (their Cholesky factor); the integrands are smooth for t > 0. The foam is a decaying maximum after onsets that
// arrive as a Poisson process (gap hazard m = lambda / (1 - b)):
//   mean F(t) = b + lambda tau / (1 + m tau)
// mean F rises with t, so J_t follows by bisection. Stored at byte 136 (float) with b, lambda, mean F at 140..148.
// P[0] accumulator UAV (raw; FoamVariance layout), 0, target W (float), tau (s, float)
#include "Bindless.hlsli"

#define CAL_N 32
static const float kCalNode[16] = { 0.275546419, 0.827284904, 1.3809802, 1.93800491, 2.49984042, 3.06813517, 3.64478125, 4.23202111,
                                    4.83260461, 5.45003327, 6.08896431, 6.75593083, 7.46075575, 8.21972877, 9.06439921, 10.0774227 };
static const float kCalWeight[16] = { 0.21170557, 0.156538994, 0.0853448083, 0.0341098477, 0.0099034617, 0.00206205105, 0.000302557026,
                                      3.05598031e-05, 2.0596221e-06, 8.88129071e-08, 2.31251841e-09, 3.34750124e-11, 2.37806486e-13,
                                      6.75529022e-16, 5.20844959e-19, 4.12460749e-23 };  // probabilists' Gauss-Hermite, n = 32

groupshared float2 gSum[CAL_N * CAL_N];
groupshared float gLo, gHi;

// Phi with fractional error below 1.2e-7 everywhere (erfc, Numerical Recipes' Chebyshev fit): the tails matter here.
float calPhi(float x)
{
    const float z = abs(x) * 0.70710678, t = 1.0 / (1.0 + 0.5 * z);
    const float e = t * exp(-z * z - 1.26551223 +
                            t * (1.00002368 + t * (0.37409196 + t * (0.09678418 + t * (-0.18628806 + t * (0.27886807 + t * (-1.13520398 + t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))));
    return x >= 0 ? 1.0 - 0.5 * e : 0.5 * e;
}
float calNode(uint i) { return i < 16 ? -kCalNode[15 - i] : kCalNode[i - 16]; }
float calWeight(uint i) { return i < 16 ? kCalWeight[15 - i] : kCalWeight[i - 16]; }
float calRead(RWByteAddressBuffer a, uint offset) { return float(int64_t(a.Load<uint64_t>(offset))) * (1.0 / 1099511627776.0); }

[numthreads(CAL_N, CAL_N, 1)]
void main(uint2 id : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    RWByteAddressBuffer accumulator = ResourceDescriptorHeap[P[0].x];
    const float aa = calRead(accumulator, 40), bb = calRead(accumulator, 48), cc = calRead(accumulator, 56);
    const float ab = calRead(accumulator, 64), ac = calRead(accumulator, 72), bc = calRead(accumulator, 80);
    const float daa = calRead(accumulator, 88), dbb = calRead(accumulator, 96), dcc = calRead(accumulator, 104);
    const float dab = calRead(accumulator, 112), dac = calRead(accumulator, 120), dbc = calRead(accumulator, 128);
    const float target = asfloat(P[0].z), tau = asfloat(P[0].w);
    // (u, v, w) covariances and the Cholesky factor of (v, w); u's regression on the factor's unit normals.
    const float suu = aa + bb + 2 * ab, svv = aa + bb - 2 * ab, sww = cc, suv = aa - bb, suw = ac + bc, svw = ac - bc;
    if (!(suu > 0) || !(target > 0))
    {
        if (index == 0) accumulator.Store4(136, asuint(float4(-1.0e6, 0, 0, 0)));  // no breaking: a still sea or no target
        return;
    }
    const float l11 = sqrt(max(svv, 0.0)), l21 = l11 > 0 ? svw / l11 : 0.0, l22 = sqrt(max(sww - l21 * l21, 0.0));
    const float c1 = l11 > 0 ? suv / l11 : 0.0, c2 = l22 > 0 ? (suw - l21 * c1) / l22 : 0.0;
    // s: u's deviation given (v, w); floored at 1e-4 of its total only for a spectrum without directional spread.
    const float s = max(sqrt(max(suu - c1 * c1 - c2 * c2, 0.0)), 1.0e-4 * sqrt(suu));
    const float x1 = calNode(id.x), x2 = calNode(id.y), weight = calWeight(id.x) * calWeight(id.y);
    const float v = l11 * x1, w = l21 * x1 + l22 * x2, mu = c1 * x1 + c2 * x2;
    if (index == 0)
    {
        gLo = 1.0 - 12.0 * sqrt(suu);
        gHi = 1.0;
    }
    GroupMemoryBarrierWithGroupSync();
    float2 result = 0;
    [loop] for (uint iteration = 0; iteration <= 40; ++iteration)
    {
        const float t = iteration == 40 ? gLo : 0.5 * (gLo + gHi);
        const float q = 4 * t + v * v + 4 * w * w;
        float2 node = 0;  // (b, lambda) at this node
        if (q > 0)
        {
            const float r = sqrt(q);
            node.x = calPhi((r - 2 - mu) / s) - calPhi((-r - 2 - mu) / s);
            [unroll] for (int root = -1; root <= 1; root += 2)
            {
                const float u = -2 + root * r, a = 0.5 * (u + v), b = 0.5 * (u - v);
                const float g0 = 1 + b, g1 = 1 + a, g2 = -2 * w;
                const float sd2 = g0 * g0 * daa + g1 * g1 * dbb + g2 * g2 * dcc + 2 * (g0 * g1 * dab + g0 * g2 * dac + g1 * g2 * dbc);
                const float e = (u - mu) / s;
                node.y += exp(-0.5 * e * e) * 0.39894228 / s * 0.79788456 * sqrt(max(sd2, 0.0)) / (0.5 * r);
            }
            node.y *= 0.5;
        }
        gSum[index] = weight * node;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint stride = CAL_N * CAL_N / 2; stride > 0; stride >>= 1)
        {
            if (index < stride) gSum[index] += gSum[index + stride];
            GroupMemoryBarrierWithGroupSync();
        }
        result = gSum[0];
        const float breaking = min(result.x, 0.999999), onsets = result.y;
        const float mean = breaking + onsets * tau / (1 + onsets / (1 - breaking) * tau);
        GroupMemoryBarrierWithGroupSync();  // every thread has read gSum[0] and the bounds
        if (iteration == 40)
        {
            if (index == 0) accumulator.Store4(136, asuint(float4(t, breaking, onsets, mean)));
            break;
        }
        if (index == 0)
        {
            if (mean < target) gLo = t;
            else gHi = t;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
