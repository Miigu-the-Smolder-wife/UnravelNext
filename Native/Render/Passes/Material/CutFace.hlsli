// Cut face material class (A11; FEATURES_GAME 2.1 "cut 재질 클래스", 2.3 quality row "단면 재질"). Owner: render C;
// M evaluates it inside its resolve (shading hookup: render A). A fracture's cut faces (physics NP_DestructionTriangle
// cut_face, pre-fractured assets' interior faces) carry a material of the Cut class. A cut face has no authored uv, so
// its textures are projected in the fragment's object space along the three axes (triplanar; the material definition,
// exact); an edge damage layer covers a band along the cut polygon's boundary with the hull (appearance model).
//
// Class parameters (INTERFACES v1.66, scene::Material / GpuMaterial):
//   cutScale        texture scale: texture repeats per object-space metre (> 0)
//   cutDamageWidth  edge damage width, object-space metres (0 = no damage layer)
// Per triangle (cluster triangle word bits 24..26, CUT_EDGE_*): which of its edges lie on the cut polygon's boundary
// (the neighbour across the edge has another material, or there is none); the cluster builder sets them at every LOD.
//
// Projection k (k = x, y, z) maps object position p to uv_k = scale * (p.b, p.c) for the other two axes (b, c) in
// right-handed order (x: (y, z), y: (z, x), z: (x, y)), with the sign of the normal's k component flipping b so the
// texture is not mirrored on the negative side. Weights w_k = |n_k|^4 / sum (n = unit object-space geometric normal of
// the triangle: constant over a flat cut face, so the blend has no seams inside it). Tangent frame of projection k:
// tangent = d p / d u (object space), bitangent = d p / d v, normal = sign(n_k) e_k.
// Definition (the reference's, mip 0): base colour, roughness factor and metallic factor = sum w_k (tap k); shading
// normal = normalize(sum w_k r_k) with r_k = cutFaceWhiteout(texture normal of tap k, projection k, unit object-space
// interpolated normal): the tap's tangent-space xy added to the surface normal's components in projection k's frame, z
// scaling its normal component, so a flat texel gives exactly the surface normal. M's filtered version evaluates each tap's
// slope moments in that frame and blends them with w_k. Three taps per texture (FEATURES_GAME 2.2: +3 taps per pixel).
#ifndef UNX_M_CUT_FACE_HLSLI
#define UNX_M_CUT_FACE_HLSLI

#define CUT_EDGE_SHIFT 24u
#define CUT_EDGE_MASK 7u     // bit i: the edge opposite corner i (corners (i + 1) % 3 -> (i + 2) % 3) is a boundary edge
#define CUT_WEIGHT_POWER 4.0

struct CutFaceProjection
{
    float2 uv, duvdx, duvdy;  // texture coordinates and their screen derivatives (per pixel)
    float3 tangent, bitangent, normal;  // object space, unit
    float weight;             // sums to 1 over the three projections
};

// Projection k of object position p (derivatives per pixel dpdx, dpdy), unit object-space geometric normal n.
CutFaceProjection cutFaceProjection(uint k, float3 p, float3 dpdx, float3 dpdy, float3 n, float scale)
{
    const uint b = (k + 1) % 3, c = (k + 2) % 3;
    const float s = n[k] < 0 ? -1.0 : 1.0;
    CutFaceProjection o;
    o.uv = scale * float2(s * p[b], p[c]);
    o.duvdx = scale * float2(s * dpdx[b], dpdx[c]);
    o.duvdy = scale * float2(s * dpdy[b], dpdy[c]);
    o.tangent = 0;
    o.bitangent = 0;
    o.normal = 0;
    o.tangent[b] = s;
    o.bitangent[c] = 1;
    o.normal[k] = s;
    const float3 a = pow(abs(n), CUT_WEIGHT_POWER);
    o.weight = a[k] / max(a.x + a.y + a.z, 1e-30);
    return o;
}

// Whiteout blend of tangent-space normal tn (unit, z >= 0) for projection pr over unit object-space surface normal n:
// r = (tn.x + n.T, tn.y + n.B, tn.z (n.N)) in the projection's frame, returned in object space (not normalised).
float3 cutFaceWhiteout(float3 tn, CutFaceProjection pr, float3 n)
{
    return (tn.x + dot(n, pr.tangent)) * pr.tangent + (tn.y + dot(n, pr.bitangent)) * pr.bitangent + tn.z * dot(n, pr.normal) * pr.normal;
}

// Distance (object-space metres) from the point with barycentrics bary on triangle (p0, p1, p2) to the nearest of its
// boundary edges (edgeMask, CUT_EDGE_*); +inf without one. Edge i is opposite corner i: distance = bary_i x height_i,
// height_i = 2 area / |edge i|. Near a boundary vertex where two boundary edges of different triangles meet, the other
// triangle's edge may be nearer than this triangle's (the band narrows there by at most the angle's sine): part of the
// appearance model.
float cutFaceEdgeDistance(float3 bary, float3 p0, float3 p1, float3 p2, uint edgeMask)
{
    const float area2 = length(cross(p1 - p0, p2 - p0));
    const float3 lengths = float3(length(p2 - p1), length(p0 - p2), length(p1 - p0));
    float d = asfloat(0x7F800000u);
    [unroll] for (uint i = 0; i < 3; ++i)
        if ((edgeMask >> i) & 1u) d = min(d, max(bary[i], 0.0) * area2 / max(lengths[i], 1e-30));
    return d;
}

// Value noise in [0, 1] at object position q (hash of the integer lattice, quintic interpolation): deterministic, the
// same on every GPU and in the CPU references (CutFace.h mirrors it bit for bit up to float rounding).
float cutFaceHash(int3 c)
{
    uint h = (uint)c.x * 0x8da6b343u ^ (uint)c.y * 0xd8163841u ^ (uint)c.z * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (h & 0xFFFFFFu) / 16777215.0;
}

float cutFaceNoise(float3 q)
{
    const float3 f = floor(q), t = q - f, u = t * t * t * (t * (t * 6 - 15) + 10);
    const int3 c = (int3)f;
    float v = 0;
    [unroll] for (uint k = 0; k < 8; ++k)
    {
        const int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        const float3 w = lerp(1 - u, u, (float3)o);
        v += w.x * w.y * w.z * cutFaceHash(c + o);
    }
    return v;
}

// Edge damage coverage in [0, 1]: the band of the cut polygon's boundary whose width is modulated by object-space noise
// (three octaves, cell = width) so the chipped edge is irregular, filtered over the pixel's footprint (object metres
// per pixel) as a box: coverage = saturate(0.5 + (threshold - distance) / footprint).
float cutFaceDamage(float3 p, float distance, float width, float footprint)
{
    if (!(width > 0) || !(distance < asfloat(0x7F800000u))) return 0;
    const float3 q = p / width;
    const float n = 0.5 * cutFaceNoise(q) + 0.3 * cutFaceNoise(2.03 * q + 17.1) + 0.2 * cutFaceNoise(4.01 * q + 41.7);
    const float threshold = width * (0.35 + 0.65 * n);
    return saturate(0.5 + (threshold - distance) / max(footprint, 1e-9));
}

// The damage layer's effect on the Standard parameters (appearance model: exposed, crushed material is darker and
// rougher): base colour x (1 - 0.45 damage), perceptual roughness toward 1 by 0.6 damage, metallic unchanged.
void cutFaceApplyDamage(float damage, inout float3 baseColor, inout float roughness)
{
    baseColor *= 1 - 0.45 * damage;
    roughness = lerp(roughness, 1.0, 0.6 * damage);
}

#endif
