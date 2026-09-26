// The pixel footprint of a water sample's refracted view (W2 finding; engine 1 agreed, FEATURES_GAME 1.9): ray
// differentials (Igehy 1999) carried from the pixel ray through the surface's interpolated normal, Snell's law and the
// transfer to the band A surface, projected to the screen - so the refracted image is integrated over what the pixel
// really sees instead of point-sampled. A rippled surface is a lens: it minifies what lies behind it, without bound at
// its fold lines (where the refraction's Jacobian is singular), and a single mip-0 sample there aliases (a lattice at
// the pixel scale inside every ring). The same normal differential widens the reflection's cone lookup.
// Exact to first order in the pixel offset (the differentials are the exact derivatives of the sample's own chain:
// triangle plane, barycentric normal interpolation, Snell, band A tangent plane, projection); the integral is a box
// filter over the parallelogram the pixel maps to, taken with trilinear taps of a 2 x 2 box mip pyramid.
#ifndef UNX_WATER_FOOTPRINT_HLSLI
#define UNX_WATER_FOOTPRINT_HLSLI

#define WATER_FOOTPRINT_TAPS 16u  // taps along the footprint's long axis at most (beyond: the level rises to keep it covered)

struct WaterDifferentials
{
    float3 dPx, dPy;  // surface point per pixel (world)
    float3 dNx, dNy;  // unit normal per pixel (the camera side's)
    float3 dDx, dDy;  // unit incident direction per pixel
};

// The pixel ray P = O + s D (D unnormalised, Dx / Dy its per-pixel derivatives: mPixelRay) meets the triangle plane with
// normal g at P; the interpolated normal is normalize(m), m = na (1 - wb - wc) + nb wb + nc wc over edges e1 = b - a,
// e2 = c - a. sideSign: +1 when the camera sees the side the normal points to, else -1.
WaterDifferentials waterDifferentials(float3 D, float3 Dx, float3 Dy, float s, float3 e1, float3 e2, float3 na, float3 nb, float3 nc, float3 m,
                                      float sideSign)
{
    WaterDifferentials w;
    const float3 g = cross(e1, e2);
    const float gD = dot(g, D), gg = dot(g, g);
    w.dPx = s * (Dx - D * (dot(g, Dx) / gD));
    w.dPy = s * (Dy - D * (dot(g, Dy) / gD));
    const float mLen = length(m);
    const float3 n = m / mLen;
    // barycentric derivatives on the plane, the normal's
    const float bx = dot(cross(w.dPx, e2), g) / gg, cx = dot(cross(e1, w.dPx), g) / gg;
    const float by = dot(cross(w.dPy, e2), g) / gg, cy = dot(cross(e1, w.dPy), g) / gg;
    const float3 mx = (nb - na) * bx + (nc - na) * cx, my = (nb - na) * by + (nc - na) * cy;
    w.dNx = sideSign * (mx - n * dot(n, mx)) / mLen;
    w.dNy = sideSign * (my - n * dot(n, my)) / mLen;
    const float dLen = length(D);
    const float3 d = D / dLen;
    w.dDx = (Dx - d * dot(d, Dx)) / dLen;
    w.dDy = (Dy - d * dot(d, Dy)) / dLen;
    return w;
}

// Derivative of the refracted direction t = eta d + (eta ci - ct) nv (d incident, nv the camera side's unit normal,
// ci = -d.nv, ct = sqrt(1 - eta^2 (1 - ci^2))) for derivatives dd of d and dn of nv.
float3 waterRefractDerivative(float3 d, float3 nv, float eta, float3 dd, float3 dn)
{
    const float ci = -dot(d, nv), ct = sqrt(max(1 - eta * eta * (1 - ci * ci), 1e-12));
    const float dci = -(dot(dd, nv) + dot(d, dn)), dct = eta * eta * ci * dci / ct;
    return eta * dd + (eta * dci - dct) * nv + (eta * ci - ct) * dn;
}

// The refracted point H = P + L t on the band A surface (unit normal nA there): its derivative for dP, dt, and the path
// length's (dL). A path grazing the band A surface (|nA.t| tiny) is held at 1e-3 (the footprint is then long, capped by
// the taps' level rule).
float3 waterTransfer(float3 dP, float3 dt, float3 t, float L, float3 nA, out float dL)
{
    const float nt = dot(nA, t);
    dL = -dot(nA, dP + L * dt) / (abs(nt) > 1e-3 ? nt : (nt < 0 ? -1e-3 : 1e-3));
    return dP + L * dt + dL * t;
}

// The half-angle (rad) the mirror direction r = d - 2 (d.nv) nv sweeps over the pixel footprint: half the larger of
// |dr/dx|, |dr/dy| (r is a unit vector: its derivative's length is the angular rate). The reflection's cone lookup and
// the sun's lobe take max(their lobe, this) so a ripple's pixel-scale normal change is integrated, not point-sampled.
float waterReflectionSpread(float3 d, float3 nv, WaterDifferentials w)
{
    const float dn = dot(d, nv);
    const float3 rx = w.dDx - 2 * ((dot(w.dDx, nv) + dot(d, w.dNx)) * nv + dn * w.dNx);
    const float3 ry = w.dDy - 2 * ((dot(w.dDy, nv) + dot(d, w.dNy)) * nv + dn * w.dNy);
    return 0.5 * max(length(rx), length(ry));
}

// Screen derivative (pixels) of a world point's projection for its world derivative dH.
float2 waterScreenDerivative(float3 H, float3 dH)
{
    const float4 c = mul(g_viewProj, float4(H, 1)), dc = mul(g_viewProj, float4(dH, 0));
    const float2 dndc = (dc.xy * c.w - c.xy * dc.w) / (c.w * c.w);
    return float2(0.5 * g_viewWidth * dndc.x, -0.5 * g_viewHeight * dndc.y);
}

// The band A surface's unit normal at screen position q (pixels) from its depth: the world points of the texel and its
// +x / +y neighbours (P = camera + D(q) z, D's view-axis component 1), facing the camera.
float3 waterBandANormal(Texture2D<float> bandADepth, float2 q)
{
    const int2 hi = int2(g_viewWidth, g_viewHeight) - 2;
    const int2 p0 = clamp(int2(q), 0, hi);
    float3 W[3];
    const int2 offs[3] = { int2(0, 0), int2(1, 0), int2(0, 1) };
    [unroll] for (int k = 0; k < 3; ++k)
    {
        const int2 p = p0 + offs[k];
        float3 D, Dx, Dy;
        mPixelRay(float2(p) + 0.5, D, Dx, Dy);
        W[k] = g_cameraPosition + D * (g_nearPlane / max(bandADepth[p], 1e-30));
    }
    float3 n = cross(W[1] - W[0], W[2] - W[0]);
    const float len = length(n);
    n = len > 0 ? n / len : -g_view[2].xyz;
    return dot(n, g_cameraPosition - W[0]) >= 0 ? n : -n;
}

// The footprint's taps: the parallelogram {u a + v b : |u|, |v| <= 1/2} (a, b: the refracted view's screen derivatives
// per output pixel, in band A pixels) covered by `count` taps along its longer side at mip level `lod` (the texel as wide
// as the shorter width, never below 1 px; more than WATER_FOOTPRINT_TAPS raise the level instead). Tap i sits at
// offset(i) = ((i + 0.5) / count - 0.5) x major; `along` is that tap's u or v coordinate (for per-tap quantities).
struct WaterFootprint
{
    float2 major;
    float lod;
    uint count;
    bool majorIsX;  // the long side is a (the x derivative)
};
WaterFootprint waterFootprint(float2 a, float2 b)
{
    WaterFootprint f;
    const float la = length(a), lb = length(b);
    f.majorIsX = la >= lb;
    f.major = f.majorIsX ? a : b;
    const float majorLen = max(la, lb), area = abs(a.x * b.y - a.y * b.x);
    const float width = majorLen > 0 ? area / majorLen : 0;  // the parallelogram's width across the long side
    const float texel = max(width, 1.0);
    float n = ceil(majorLen / texel);
    float lodTexel = texel;
    if (n > WATER_FOOTPRINT_TAPS)
    {
        lodTexel = majorLen / WATER_FOOTPRINT_TAPS;
        n = WATER_FOOTPRINT_TAPS;
    }
    f.count = uint(max(n, 1.0));
    f.lod = log2(lodTexel);
    return f;
}
float2 waterFootprintOffset(WaterFootprint f, uint i, out float along)
{
    along = (float(i) + 0.5) / float(f.count) - 0.5;
    return along * f.major;
}
#endif
