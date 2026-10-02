// unx-kernel: cs_6_6 main
// Diaphragm depth of field, the bokeh's shape as two 32 x 32 tables (DiaphragmDof.cpp has the diaphragm's model; the
// reference's DOFBokehLUT.usf). The diaphragm is P[0].z blades: straight ones (a polygon) or arcs of radius P[2].x about
// a point P[2].y from the centre (a lens stopped down a little from its widest aperture); the shape's area is the unit
// disc's whatever the blades, so a radius means the same energy.
//   edge table    (R16F, centred on the texel origin, wrapping) per direction, the distance of the shape's edge as a
//                 factor of the radius: the sprites' and the full-resolution gather's intersection (DdofCommon.hlsli
//                 ddofEdgeFactor);
//   gather table  (RG16F, centred on texel 16) per sample of the gather kernel's square rings (ring r, sample s at
//                 texel 16 + its square position), the sample's place in kernel units x r: the ring's samples spread
//                 evenly along the shape's outline (DdofGather.hlsl).
// P[0] = { edge table UAV, gather table UAV, blade count, 0 }
// P[1] = { asuint(radius to circumscribed radius), asuint(radius to incircle radius), asuint(rotation), 0 }
// P[2] = { asuint(blade radius) (0: straight blades), asuint(blade centre offset), 0, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/DdofCommon.hlsli"

float edgeFactor(float angle)
{
    const float blades = (float)P[0].z, radius = asfloat(P[2].x), offset = asfloat(P[2].y);
    const float edge = floor(angle / (2.0 * DDOF_PI) * blades);
    const float alpha = angle - (edge + 0.5) * (2.0 * DDOF_PI) / blades;  // (from the blade's middle)
    if (radius <= 0.0) return asfloat(P[1].y) / cos(alpha);
    // the arc (x + offset)^2 + y^2 = radius^2 along the direction: its positive root
    const float b = 2.0 * offset * cos(alpha), c = offset * offset - radius * radius;
    return (-b + sqrt(b * b - 4.0 * c)) * 0.5;
}

// A point of the square ring (the kernel's sample layout) as the ring's radius and the sample's index along it.
void ringCoordinate(float2 c, out float radius, out float index)
{
    float quarter = 0;
    [unroll] for (uint i = 0; i < 3; ++i)
        if (c.x < 0.0 || c.y < 0.0)
        {
            c = float2(c.y, -c.x);
            quarter += 1;
        }
    radius = max(abs(c.x), abs(c.y));
    index = quarter * 2 * radius + (c.x > c.y ? c.y : 2 * radius - c.x);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= DDOF_LUT)) return;
    const float blades = (float)P[0].z, circumscribed = asfloat(P[1].x), rotation = asfloat(P[1].z);
    const float bladeRadius = asfloat(P[2].x), bladeOffset = asfloat(P[2].y);

    float2 fromOrigin = float2(id) + 0.5;
    if (fromOrigin.x > DDOF_LUT / 2) fromOrigin.x -= DDOF_LUT;
    if (fromOrigin.y > DDOF_LUT / 2) fromOrigin.y -= DDOF_LUT;
    RWTexture2D<float> edges = ResourceDescriptorHeap[P[0].x];
    edges[id] = edgeFactor(atan2(fromOrigin.y, fromOrigin.x) + rotation + DDOF_PI);

    float ring, index;
    ringCoordinate(float2(id) - DDOF_LUT / 2, ring, index);
    const float along = ((index + frac(ring * 0.5)) / max(1.0, ring * 8.0)) * blades + 0.5 * blades;  // (in blades; + half a turn)
    const float edge = floor(along), within = frac(along), bladeAngle = 2.0 * DDOF_PI / blades;
    float2 unit;
    if (bladeRadius <= 0.0)
    {
        const float a0 = edge * bladeAngle - rotation, a1 = a0 + bladeAngle;
        unit = circumscribed * lerp(float2(cos(a0), sin(a0)), float2(cos(a1), sin(a1)), within);
    }
    else
    {
        // along the blade's arc, then from the blade's frame into the kernel's
        const float beta = asin(saturate(circumscribed / bladeRadius * sin(DDOF_PI / blades)));
        const float onArc = beta * (within * 2.0 - 1.0);
        const float2 blade = float2(bladeRadius * cos(onArc) - bladeOffset, bladeRadius * sin(onArc));
        const float turn = (edge + 0.5) * bladeAngle - rotation;
        unit = float2(blade.x * cos(turn) - blade.y * sin(turn), blade.x * sin(turn) + blade.y * cos(turn));
    }
    RWTexture2D<float2> samples = ResourceDescriptorHeap[P[0].y];
    samples[id] = unit * ring;
}
