// The lens projection of the output picture (output.lens_panini_d; Upscale.cpp upscaleLens; the reference's Panini
// projection - PaniniProjection.ush/.usf, r.LensDistortion.Panini.D / S, after Sharpless et al., "Pannini: A New Projection
// for Rendering Wide Angle Perspective Images"): a wide field of view rendered rectilinear stretches what is near the
// picture's sides; projected instead from a point d behind the view's centre onto the cylinder about the vertical axis,
// the sides keep their proportions and verticals stay straight (d = 1: the cylindrical stereographic projection; s fades
// in the compression of the verticals that also keeps horizontals through the centre straight). The picture is scaled
// so the rendered view's width fills it ('scale': the centre is then magnified by it).
// The temporal upscale's history is the lens picture (TsrUpdate.hlsl): each history pixel takes its samples at its place
// in the rendered picture, so the magnified centre is reconstructed from the jittered samples as any upscale is - no
// resolution is lost to a resample of the finished picture. The reference does the same in its TSR (through displacement
// tables; here the projection and its inverse are evaluated).
// lens = { tan(half field of view x), tan(half field of view y), d, s } of the rendered view; d <= 0: no lens.
#ifndef UNX_LENS_HLSLI
#define UNX_LENS_HLSLI

// A view direction (x, y, -1) to its place on the lens plane.
float2 lensPanini(float2 om, float d, float s)
{
    const float invLength = rsqrt(1.0 + om.x * om.x);
    const float sinPhi = om.x * invLength, tanTheta = om.y * invLength, cosPhi = sqrt(saturate(1.0 - sinPhi * sinPhi));
    return (d + 1.0) / (d + cosPhi) * float2(sinPhi, lerp(tanTheta, tanTheta / cosPhi, s));
}

// A place on the lens plane back to its view direction: the line from the projection centre through it meets the unit
// circle (the cylinder seen from above) at the direction's azimuth.
float2 lensPaniniInverseRoot(float2 on, float d, float s)
{
    const float A = 1.0 + d, B = -on.x, C = on.x * d;
    const float a = 1.0 + (B * B) / (A * A), b = 2.0 * (B * C) / (A * A), c = (C * C) / (A * A) - 1.0;
    const float z = (-b - sqrt(max(b * b - 4.0 * a * c, 0.0))) / (2.0 * a);
    const float cosPhi = -z, sinPhi = sqrt(saturate(1.0 - cosPhi * cosPhi)) * sign(on.x);
    const float omx = sinPhi / cosPhi;
    const float tanTheta = on.y / ((d + 1.0) / (d + cosPhi) * lerp(1.0, 1.0 / cosPhi, s));
    return float2(omx, tanTheta * sqrt(1.0 + omx * omx));
}
float2 lensPaniniInverse(float2 on, float d, float s)
{
    float2 om = lensPaniniInverseRoot(on, d, s);
    // (near the centre column the root has no precision left for x: linear from its value a little aside)
    const float aside = 0.003;
    if (abs(on.x) < aside) om.x = (abs(on.x) / aside) * lensPaniniInverseRoot(float2(aside * sign(on.x), on.y), d, s).x;
    return om;
}

// UV of the lens picture <-> UV of the rendered (rectilinear) picture.
float2 lensToRendered(float2 uv, float4 lens, float scale)
{
    if (!(lens.z > 0.0 && scale > 0.0)) return uv;
    return lensPaniniInverse((uv * 2.0 - 1.0) * lens.xy / scale, lens.z, lens.w) / lens.xy * 0.5 + 0.5;
}
float2 renderedToLens(float2 uv, float4 lens, float scale)
{
    if (!(lens.z > 0.0 && scale > 0.0)) return uv;
    return lensPanini((uv * 2.0 - 1.0) * lens.xy, lens.z, lens.w) / lens.xy * (scale * 0.5) + 0.5;
}

#endif
