// unx-kernel: cs_6_6 main
// Depth of field gather (A5 lens integral, FEATURES_GAME 4.1; DepthOfField.cpp). The thin lens over the pinhole image: a
// pixel q at signed circle-of-confusion radius rho_q (px) is seen through the lens from every point of its disk, the
// nearest surface first. Per output pixel p and radius octave c whose sources reach p's tile (DofReach), the sources are
// read at pyramid level c (octave 0: the input image and its radii, full resolution) over the lattice offsets from the
// texel holding p out to reach / 2^c + 1/2 (+1.92 off level 0: the centroid and p anywhere in their texels, half the
// spread) - at most DOF_OFFSETS per octave. A source (a level-0 pixel, or a level-c texel's octave-c pixels: area share
// alpha, radiance L, radius rho, centroid, spread w; DofCommon.hlsli) spreads alpha 4^c L over its disk:
//   r = max(|rho|, 1/2), coverage of p = sat((r - d) / w + 1/2) (d from the centroid), lens share f = alpha 4^c / A
//   (A = the pixel lattice's area of a level-0 disk, dofDiskArea; pi max(r, w/2)^2 + pi w^2 / 12 above level 0);
//   against p's surface plane in (x, y, rho) (rho_p + the local gradient x the offset: exact for planes), sources nearer
//   by more than max(2^c / 2, 0.02 |rho_p|) form the front layer F (weight W_F: the share of the lens they cover); the rest
//   (p's surface and what lies behind it) the back layer B, where p's own surface bounds the disks of the sources behind
//   it to |rho_p|: a sharp surface hides the blurred background behind it;
//   out = F / max(1, W_F) + (1 - min(1, W_F)) B / W_B.
// Exact where no surface hidden in the pinhole image shows through the lens; B's normalisation fills those holes with the
// neighbouring background (V's conditional second layer is the exact form). A pixel no blurred source reaches is copied:
// in-focus regions are bit-identical to the input.
// P[0] = { colour SRV, coc SRV, reach SRV (raw), destination UAV }, P[1] = { width, height, tilesX, statistics UAV (raw) }
// P[2] = { A1, A2, A3, A4 }, P[3] = { A5, A6, A7, S1 }, P[4] = { S2, S3, S4, S5 }, P[5] = { S6, S7, 0, 0 } (level-c SRVs)
#include "Bindless.hlsli"
#include "Passes/Shading/DofCommon.hlsli"

uint srvA(uint c)
{
    return c == 1 ? P[2].x : c == 2 ? P[2].y : c == 3 ? P[2].z : c == 4 ? P[2].w : c == 5 ? P[3].x : c == 6 ? P[3].y : P[3].z;
}
uint srvS(uint c)
{
    return c == 1 ? P[3].w : c == 2 ? P[4].x : c == 3 ? P[4].y : c == 4 ? P[4].z : c == 5 ? P[4].w : c == 6 ? P[5].x : P[5].y;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 size = P[1].xy;
    if (any(id >= size)) return;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> coc = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer reachBuffer = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    const float4 own = colour.Load(int3(id, 0));
    const uint tile = (id.y / DOF_TILE) * P[1].z + id.x / DOF_TILE;
    float reach[DOF_CATEGORIES];
    float widest = -1;
    [unroll] for (uint k = 0; k < DOF_CATEGORIES; ++k)
    {
        reach[k] = asfloat(reachBuffer.Load(4 * (tile * DOF_CATEGORIES + k)));
        widest = max(widest, reach[k]);
    }
    if (widest <= 0)  // every source that reaches p is exactly in focus (or none is)
    {
        output[id] = own;
        return;
    }
    const float2 p = float2(id) + 0.5f;
    const float rhoP = coc.Load(int3(id, 0));
    const float bound = abs(rhoP);  // p's own surface bounds the back layer's disks
    // p's surface as a plane in (x, y, rho): rho is linear in the screen over any plane (rho is linear in 1/z), so a source
    // on p's surface is predicted exactly by the local gradient (one-sided differences, the smaller: not across an edge)
    float2 gradient = 0;
    {
        const int2 ip = int2(id);
        const float l = ip.x > 0 ? rhoP - coc.Load(int3(ip - int2(1, 0), 0)) : asfloat(0x7F800000u);
        const float r = ip.x + 1 < (int)size.x ? coc.Load(int3(ip + int2(1, 0), 0)) - rhoP : asfloat(0x7F800000u);
        const float u = ip.y > 0 ? rhoP - coc.Load(int3(ip - int2(0, 1), 0)) : asfloat(0x7F800000u);
        const float v = ip.y + 1 < (int)size.y ? coc.Load(int3(ip + int2(0, 1), 0)) - rhoP : asfloat(0x7F800000u);
        gradient.x = abs(l) < abs(r) ? l : (abs(r) < asfloat(0x7F800000u) ? r : 0);
        gradient.y = abs(u) < abs(v) ? u : (abs(v) < asfloat(0x7F800000u) ? v : 0);
    }
    float3 front = 0, back = 0;
    float wFront = 0, wBack = 0;
    [unroll] for (uint c = 0; c < DOF_CATEGORIES; ++c)
    {
        if (reach[c] < 0 && c != 0) continue;  // (octave 0 always: the pixel's own tap)
        const float s = (float)(1u << c);
        const float limit = c == 0 ? max(reach[0], 0.0f) + 1.4143f : reach[c] / s + 1.92f;
        const int2 levelSize = int2((size + (1u << c) - 1) >> c);
        const int2 base = int2(id >> c);
        const float delta = max(0.5f * s, 0.02f * abs(rhoP));
        for (uint i = 0; i < DOF_OFFSETS; ++i)
        {
            const int2 o = kDofOffsets[i];
            if (length(float2(o)) > limit) break;
            const int2 t = base + o;
            if (any(t < 0) || any(t >= levelSize)) continue;
            float3 L;
            float rho, residual, wFrontTap, wBackTap;  // residual: rho less p's surface plane there
            float2 sourcePosition = float2(t) + 0.5f;  // (level 0: the pixel centre; above: the texel's centroid)
            if (c == 0)
            {
                rho = coc.Load(int3(t, 0));
                if (dofCategory(rho) != 0) continue;
                L = colour.Load(int3(t, 0)).rgb;
                // the exact pixel kernel; p's own surface bounds the spread of a source behind it to its own radius
                residual = rho - (rhoP + dot(gradient, float2(t - int2(id))));
                wFrontTap = dofPixelWeight(-o, abs(rho));
                wBackTap = residual > delta ? dofPixelWeight(-o, min(abs(rho), abs(rhoP))) : wFrontTap;
            }
            else
            {
                Texture2D<float4> a = ResourceDescriptorHeap[srvA(c)];
                Texture2D<float4> shape = ResourceDescriptorHeap[srvS(c)];
                const float4 ca = a.Load(int3(t, 0));
                const float alpha = ca.w;
                if (!(alpha > 0)) continue;
                const float4 sh = shape.Load(int3(t, 0));
                rho = sh.w / alpha;
                L = ca.rgb / alpha;
                const float2 q = (float2(t) + 0.5f) * s + sh.xy;
                sourcePosition = q;
                const float d = length(q - p);
                residual = rho - (rhoP + dot(gradient, q - p));
                const float w = sqrt(sh.z * sh.z + 1.0f);  // the source's spread and the receiver pixel (box variances add)
                const float r = abs(rho), rw = max(r, 0.5f * w);
                const float f = alpha * s * s / (3.14159265f * (rw * rw + w * w / 12.0f));
                wFrontTap = f * saturate((r - d) / w + 0.5f);
                wBackTap = residual > delta ? f * saturate((min(r, bound) - d) / w + 0.5f) : wFrontTap;
            }
            if (abs(residual) <= delta && abs(rho) > 0.5f)
            {
                // a source on p's plane: its pixel is a tilted patch, seen from lens point u = (p - q) / rho through the
                // area factor det(I + u grad rho^T) = 1 + u . grad rho (the kernel's odd part: its energy is unchanged)
                const float m = max(1.0f + dot(p - sourcePosition, gradient) / rho, 0.0f);
                wFrontTap *= m;
                wBackTap *= m;
            }
            if (residual < -delta)
            {
                front += wFrontTap * L;
                wFront += wFrontTap;
            }
            else
            {
                back += wBackTap * L;
                wBack += wBackTap;
            }
        }
    }
    const float3 b = wBack > 0 ? back / wBack : own.rgb;
    output[id] = float4(front / max(1.0f, wFront) + (1.0f - min(1.0f, wFront)) * b, own.a);
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[1].w];
    const uint n = WaveActiveCountBits(true);
    if (WaveIsFirstLane()) stats.InterlockedAdd(0, n);
}
