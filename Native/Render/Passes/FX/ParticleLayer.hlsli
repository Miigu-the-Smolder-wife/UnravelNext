// FX particle layer: what the shading composite reads (request 20260926_FX_particle_render_pass.md 4; public header of the
// FX track, included by M). Needs the view's frame constants (Common/Frame.hlsli) for the view size.
//
//   view.particleLayer      Texture2D<float4>, ceil(W/4) x ceil(H/4): premultiplied radiance L (x exposure; the air
//                           between the camera and each particle applied) and transmittance T of the particles in front
//                           of the layer pixel's opaque surfaces, front to back.
//   view.particleDepthRange Texture2D<float2>, same size: (farthest, nearest) device depth of the particles that
//                           contribute to the layer pixel ((0, 0): none).
//   view.particleEdges      ByteAddressBuffer: word 0 = edge block count; words 4 .. 4 + lw * lh = per layer pixel the
//                           index of its edge block or FX_PARTICLE_NO_EDGE; then at fxParticleEdgeBlocksOffset() the edge
//                           blocks, 128 B each: the 16 full-resolution pixels of the layer pixel's 4 x 4 block (row major),
//                           half4 (L, T) each, computed at full resolution.
//
// A layer pixel is an edge block when the 1/4 resolution is not in band there: a particle's depth lies between the
// block's nearest and farthest opaque depth (the particle is in front of some of its pixels only), or a small particle
// (radius < 80 full-resolution pixels, ParticleLayerPass.hlsli) touches it. Composite (in shading, before the tone map):
//   C = C_surface x T + L,  (L, T) = fxParticleLayerAt(pixel).
#ifndef FX_PARTICLE_LAYER_HLSLI
#define FX_PARTICLE_LAYER_HLSLI

#define FX_PARTICLE_NO_EDGE 0xFFFFFFFFu

uint2 fxParticleLayerSize() { return uint2((g_viewWidth + 3u) / 4u, (g_viewHeight + 3u) / 4u); }
uint fxParticleEdgeBlocksOffset()
{
    const uint2 s = fxParticleLayerSize();
    return (16u + 4u * s.x * s.y + 15u) & ~15u;
}
float4 fxParticleHalf4(uint2 v) { return float4(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y), f16tof32(v.y >> 16)); }

// (L, T) of full-resolution pixel 'pixel': the edge block's value when its layer pixel is an edge block, else the bilinear
// reconstruction of the layer over the non-edge layer pixels of the 2 x 2 footprint (an edge block's layer value is not
// a sample of the band-limited field; the layer pixel of 'pixel' itself is in the footprint).
float4 fxParticleLayerAt(Texture2D<float4> layer, ByteAddressBuffer edges, uint2 pixel)
{
    const uint2 size = fxParticleLayerSize();
    const uint2 lp = pixel / 4u;
    const uint index = edges.Load(16u + 4u * (lp.y * size.x + lp.x));
    if (index != FX_PARTICLE_NO_EDGE)
    {
        const uint2 q = pixel & 3u;
        return fxParticleHalf4(edges.Load2(fxParticleEdgeBlocksOffset() + index * 128u + 8u * (q.y * 4u + q.x)));
    }
    const float2 sp = (float2(pixel) + 0.5f) / 4.0f - 0.5f;
    const float2 f = sp - floor(sp);
    const int2 b = (int2)floor(sp);
    float4 sum = 0;
    float weight = 0;
    [unroll] for (uint k = 0; k < 4u; ++k)
    {
        const int2 o = int2(k & 1u, k >> 1);
        const int2 t = clamp(b + o, int2(0, 0), int2(size) - 1);
        if (edges.Load(16u + 4u * ((uint)t.y * size.x + (uint)t.x)) != FX_PARTICLE_NO_EDGE) continue;
        const float wk = (o.x ? f.x : 1.0f - f.x) * (o.y ? f.y : 1.0f - f.y);
        sum += wk * layer.Load(int3(t, 0));
        weight += wk;
    }
    return weight > 0 ? sum / weight : layer.Load(int3(lp, 0));
}
#endif
