// Particle render pass (G7; request 20260926_FX_particle_render_pass.md), shared declarations of its kernels. The view's
// frame constants are the root CBV b1 (Common/Frame.hlsli); the pass's own values are one LayerConstants record whose
// index is P[0].x. P[0].y, P[0].z: fx.particles.soft and near_fade (FxLayerTile.hlsl). P[0].w: the frame's LayerExtra
// record (the sprite looks, the previous view for the particles' motion, the layer's motion target).
//
// Screen space: full-resolution pixel i covers [i, i + 1) (centre i + 0.5); the layer pixel (lx, ly) covers the 4 x 4 block
// [4 lx, 4 lx + 4) x [4 ly, 4 ly + 4) and is sampled at its centre (4 lx + 2, 4 ly + 2); a tile is 8 x 8 layer pixels
// (32 x 32 full-resolution pixels). Depth is the device depth of the reversed-Z infinite projection (near / view distance:
// larger = closer).
#ifndef FX_PARTICLE_LAYER_PASS_HLSLI
#define FX_PARTICLE_LAYER_PASS_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/FX/StreamRecords.hlsli"
#include "Passes/FX/ParticleLayer.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"

#define FX_LAYER_TILE 8u         // layer pixels per tile side
#define FX_LAYER_SCALE 4u        // full-resolution pixels per layer pixel side
#define FX_LAYER_TILE_ENTRIES 2048u  // sorted entries a tile holds in group memory (more: FX_LAYER_STATUS_TILE_OVERFLOW)
// A sprite is drawn in the 1/4 layer only when its radius is at least this many full-resolution pixels: the layer's
// bilinear reconstruction of the sprite profile a (1 - q)^2 (q = d^2 / r^2) errs by at most h^2 / 8 max|a''| = 1.5 (h / r)^2
// of its opacity (h = 4 px the layer spacing), so r >= 80 px keeps it <= 1/256; smaller sprites mark their blocks for the
// full-resolution walk (ParticleLayer.hlsli edge blocks).
#define FX_LAYER_MIN_RADIUS 80.0f

// counters[] of the pass
#define FX_LAYER_COUNTER_ENTRIES 0u    // tile entries of the frame (FxLayerScan)
#define FX_LAYER_COUNTER_SPARE 1u      // (the edge block count is word 0 of the edge buffer)
#define FX_LAYER_COUNTER_STATUS 2u     // status bits
#define FX_LAYER_COUNTER_DRAWN 3u      // records drawn (visible after the cull)
#define FX_LAYER_STATUS_ENTRY_OVERFLOW 1u   // the tile entries exceed the entry buffer (entries dropped: a defect)
#define FX_LAYER_STATUS_TILE_OVERFLOW 2u    // a tile holds more than FX_LAYER_TILE_ENTRIES entries (the farthest dropped)
#define FX_LAYER_STATUS_EDGE_OVERFLOW 4u    // the edge blocks exceed their buffer (those blocks keep the layer value)
#define FX_LAYER_STATUS_RANGE 8u            // an index outside its buffer
#define FX_LAYER_STATUS_MATERIAL 16u        // a sprite program's material is neither 0 (emissive), 1 (lit) nor a set look: not drawn

// ---- sprite looks (unx/fx/SpriteLooks.h): a program whose material is 2 + i is drawn with look i of the table.
struct FxSpriteLook  // 80 B (SpriteLooks.cpp LookGpu)
{
    uint texture, normalTexture, motionTexture, flags;  // SRVs (UNX_NONE: none); FX_LOOK_*
    float motionScale, aspect, stretch, stretchMax;
    float2 pivot; float rotationRate, shadowDensity;
    float3 axis; float pad0;
    float2 textureSize; float pad1, pad2;
};
#define FX_LOOK_VALID 1u
#define FX_LOOK_LIT (1u << 6)
#define FX_LOOK_FRAME_BLEND (1u << 9)
#define FX_LOOK_FRAMES_OVER_LIFE (1u << 10)
#define FX_LOOK_SMOOTH (1u << 11)
#define FX_LOOK_SHADOW (1u << 12)
#define FX_BLEND_ALPHA 0u
#define FX_BLEND_ADDITIVE 1u
#define FX_BLEND_PREMULTIPLIED 2u
#define FX_FACING_CAMERA_PLANE 0u
#define FX_FACING_CAMERA_POSITION 1u
#define FX_FACING_VELOCITY 2u
#define FX_FACING_AXIS 3u
#define FX_NORMAL_NONE 0u
#define FX_NORMAL_SPHERICAL 1u
#define FX_NORMAL_MAP 2u
uint fxLookBlend(FxSpriteLook l) { return (l.flags >> 1) & 3u; }
uint fxLookFacing(FxSpriteLook l) { return (l.flags >> 3) & 3u; }
uint fxLookNormal(FxSpriteLook l) { return (l.flags >> 7) & 3u; }
uint fxLookRibbonUv(FxSpriteLook l) { return (l.flags >> 13) & 1u; }

// The frame's values beside LayerConstants (P[0].w; ParticleLayer.cpp LayerExtra).
struct LayerExtra  // 96 B
{
    uint looks, lookCount, ribbonTangents, motion;  // looks: StructuredBuffer<FxSpriteLook> (UNX_NONE: no table); the strips'
                                                    // tangents (UAV, FxRibbon); the layer's motion target (UAV, RG16F)
    float4 prevViewProj[4];                         // rows of the previous frame's unjittered view-projection (world ->
                                                    // clip); all 0: the frame has none (the records' motion is 0)
    uint shadowParams, ribbonSegments, pad1, pad2;  // the sun's particle transmittance map (ParticleShadow.hlsli; UNX_NONE:
                                                    // none); pieces a strip segment is drawn in at most (0, 1: straight)
};
LayerExtra fxLayerExtra()
{
    StructuredBuffer<LayerExtra> x = ResourceDescriptorHeap[P[0].w];
    return x[0];
}

struct LayerConstants
{
    float3 offsetCur; float w;          // stream anchor of the latest tick - camera (float of a double difference); frame
                                        // time between the previous tick's end (w = 0) and the latest tick's end (w = 1)
    float3 offsetPrev; float dt;        // the previous tick's; dt of the latest tick
    uint threads, current, rangeCount, recordCapacity;
    uint layerWidth, layerHeight, tilesX, tilesY;
    uint posAgeCur, velocityCur, posAgePrev, velocityPrev;
    uint dynamicCur, dynamicPrev, emitters, programs;
    uint curveKeys, ranges, blocks, records;
    uint tileCounts, tileStarts, tileFill, entries;
    uint depth, layer, depthRange, edgeBlocks;
    uint counters, entryCapacity, edgeCapacity, layerSrv;  // layer / edges: UAVs; layerSrv, edgeBlocksSrv: their SRVs
    uint edgeBlocksSrv, ribbonPoints, ribbonLinks, ribbonVertices;  // ribbons (strips): this frame's points, links, vertices
    uint shadowPageTable, shadowPool, shadowBlocks, shadowSearchBound;  // S's ShadowSrvs (stage 2 lighting)
    uint shadowConstants, shadowLights, shadowSlotOfLight, shadowLayers;
    uint giCache, froxelLights, airVolume, transmittance;
    uint multiScatter, ribbonAppearance, ribbonCapacity, stripBase;  // per-point FxRibbonAppearance; points; strip records at
                                                                      // stripBase + point (after the sprite records)
    uint ribbonRows;                    // per emitter row uint2 (first point, its birth: the row's dying_birth); x = none: no ribbon
    float3 streamAxes;                  // stream space -> renderer axis signs (the Unity World: (1, 1, -1)); offsets are in stream space
};

// One render range (ParticleSystem.cpp): render threads [thread, thread + count) are births [first, first + count) of
// 'row' at stateBase + (birth - first) of the latest state (bit 31 of prevCountFlags clear; the previous tick's state of
// the same particle at prevBase + (birth - prevFirst) when that is < prevCount) or of the previous state (bit 31 set:
// particles that died in the latest tick).
struct RenderRange { uint thread, count, row, stateBase; uint first, prevBase, prevFirst, prevCountFlags; };

// A drawable particle of this frame: 64 B (fx::kLayerRecordBytes).
struct LayerRecord
{
    float2 centre;        // full-resolution pixel coordinates
    float radius;         // full-resolution pixels (pixel-footprint prefiltered, ParticleLayer.hlsli); 0 = not drawn. A
                          // look's sprite: the radius of the circle around its quad
    float depth;          // device depth of the particle centre
    uint2 radianceAlpha;  // half4: radiance x exposure (the air in front applied), opacity scale. A look's sprite: the
                          // radiance before its texture and the air's in-scattering (extra)
    uint flags;           // FX_LAYER_RECORD_* | (look + 1) << 8 (0: the round profile of the built-in materials)
    uint program;
    // ---- a look's sprite (0 otherwise, but the motion)
    uint2 axes;           // half4: the quad's half axes on screen, right (xy) and up (zw), full-resolution pixels
    float frame;          // the flipbook's frame; its fraction blends toward the next one
    uint uvOffset;        // half2: the uv offset (the program's + its scroll x age)
    uint4 extra;          // halves. [0], [1] low: the light's first moment over its fluence in the sprite's frame (right,
                          // up, facing) - a look lit per pixel shows radiance x max(0, 1 + 2 moment . normal); [1] high,
                          // [2]: the air's in-scattering in front of the sprite (x exposure), added after the texture;
                          // [3]: the centre's travel on screen since the previous frame, pixels (every sprite)
};
#define FX_LAYER_RECORD_SMALL 1u  // radius < FX_LAYER_MIN_RADIUS, or a look's sprite not marked smooth: full-resolution walk only
#define FX_LAYER_RECORD_STRIP 2u  // a ribbon/beam segment: radianceAlpha = (its point, the previous point), evaluated per pixel
uint fxRecordLook(LayerRecord r) { return (r.flags >> 8) & 0xFFFu; }  // look + 1

// Ribbon points of this frame (FxLayerSetup: the particle at the frame time, camera-relative; the layout of the stream's
// ribbon points) and the strip vertices FxRibbon builds from them (two per point, the side frame parallel-transported).
// (the render pass's points: program = the point's program, look = its look + 1 (0: none) - FxLayerStrips gives them to
// the segment's record)
struct FxRibbonPoint { float3 position; float width; float age; uint valid; uint program, look; };  // 32 B (Particles.hlsli RibbonPoint)
struct FxRibbonVertex { float3 position; float3 normal; float2 uv; };                          // 32 B (FxRibbon.hlsl)
// A ribbon point's appearance (FxLayerSetup's ribbonPoint), 24 B: radianceAlpha - half4, radiance x exposure before the
// air and the look's texture, opacity; moment - half4, a look lit per pixel: the light's first moment over its
// fluence, world axes (the strip's pixel shows radiance x max(0, 1 + 2 moment . normal)), else 0; motion - half2, the
// point's travel on screen since the previous frame, pixels.
struct FxRibbonAppearance { uint2 radianceAlpha; uint2 moment; uint motion; uint pad; };

LayerConstants fxLayerConstants()
{
    StructuredBuffer<LayerConstants> c = ResourceDescriptorHeap[P[0].x];
    return c[0];
}
void fxLayerStatus(LayerConstants c, uint bits)
{
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
    InterlockedOr(counters[FX_LAYER_COUNTER_STATUS], bits);
}
float4 fxUnpackHalf4(uint2 v) { return float4(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y), f16tof32(v.y >> 16)); }
uint2 fxPackHalf4(float4 v) { return uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16)); }
float2 fxUnpackHalf2(uint v) { return float2(f16tof32(v), f16tof32(v >> 16)); }
uint fxPackHalf2(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
float3 fxRecordMoment(LayerRecord r) { return float3(f16tof32(r.extra.x), f16tof32(r.extra.x >> 16), f16tof32(r.extra.y)); }
float3 fxRecordInscatter(LayerRecord r) { return float3(f16tof32(r.extra.y >> 16), f16tof32(r.extra.z), f16tof32(r.extra.z >> 16)); }
float2 fxRecordMotion(LayerRecord r) { return fxUnpackHalf2(r.extra.w); }
uint4 fxPackExtra(float3 moment, float3 inscatter, float2 motion)
{
    return uint4(fxPackHalf2(moment.xy), f32tof16(moment.z) | (f32tof16(inscatter.x) << 16), fxPackHalf2(inscatter.yz), fxPackHalf2(motion));
}

// Tiles [t0, t1] (inclusive) a record's square [centre - radius, centre + radius] overlaps.
bool fxLayerTiles(LayerConstants c, LayerRecord r, out uint2 t0, out uint2 t1)
{
    const float span = (float)(FX_LAYER_TILE * FX_LAYER_SCALE);
    const float2 lo = floor((r.centre - r.radius) / span), hi = floor((r.centre + r.radius) / span);
    t0 = (uint2)clamp(lo, 0.0f, float2(c.tilesX - 1, c.tilesY - 1));
    t1 = (uint2)clamp(hi, 0.0f, float2(c.tilesX - 1, c.tilesY - 1));
    return r.radius > 0 && all(hi >= 0.0f) && lo.x <= (float)(c.tilesX - 1) && lo.y <= (float)(c.tilesY - 1);
}

// Opacity of a record at a point (full-resolution pixel coordinates): the sprite profile alpha (1 - q)^2, q = d^2 / r^2.
float fxLayerOpacity(LayerRecord r, float2 p, float alpha)
{
    const float2 d = p - r.centre;
    const float q = dot(d, d) / (r.radius * r.radius);
    return q < 1.0f ? alpha * (1.0f - q) * (1.0f - q) : 0.0f;
}

// Ray (origin 0, camera-relative direction D) against triangle (a, b, c): distance t along D and barycentrics (u, v) of b
// and c. Two-sided.
bool fxRayTriangle(float3 D, float3 a, float3 b, float3 c, out float t, out float u, out float v)
{
    const float3 e1 = b - a, e2 = c - a, q = cross(D, e2);
    const float det = dot(e1, q);
    t = u = v = 0;
    if (abs(det) < 1e-20f) return false;
    const float inv = 1.0f / det;
    const float3 s = -a;
    u = dot(s, q) * inv;
    const float3 r = cross(s, e1);
    v = dot(D, r) * inv;
    t = dot(e2, r) * inv;
    return u >= 0 && v >= 0 && u + v <= 1 && t > 0;
}

// A strip record at full-resolution point p: the pixel-centre ray's hit on the segment's quad (the previous point's edge
// vertices A0, A1, this point's B0, B1; triangles (A0, A1, B0) and (B0, A1, B1)), s along the segment (0 at A) and e across
// (0 at edge 0). Opacity: the points' alphas interpolated along, times the profile (1 - x^2)^2 across (x = 2 e - 1; the
// sprite profile's section); radiance: the points' interpolated along, then the air between the camera and the hit (S's air
// volume at the pixel and the hit's view depth). Device depth = near / view depth of the hit.
// Tessellation (x.ribbonSegments > 1; fx.particles.ribbon_segments - Niagara's ribbon tessellation with its curve
// through the points): the segment's centreline is the cubic through its two points with the strip's tangents there
// (FxRibbon's: the mean of the neighbouring segments' directions, x the segment's length), drawn in pieces - one per
// FX_RIBBON_PIECE_ANGLE of turn between the two tangents, x.ribbonSegments at most; the side vector and the width run
// linearly between the ends. A straight segment is one piece: the quad above.
// A look (the record's; unx/fx/SpriteLooks.h): its texture over the strip - across by e; along by the strip's distance
// over the program's ribbon_uv (the texture repeats) or by the points' age over the lifetime (once over the ribbon:
// Niagara's tiled and scaled UV modes) - its alpha in place of the profile, and its blend. Lit per pixel (the look's
// normal mode, as the sprites): the points' moments run along the segment and the pixel's normal is a tube's over the
// strip - the strip's side and its normal toward the viewer, by the place across - or the look's normal texture in the
// strip's frame (+x along it, +y across). motion: the points' travel on screen, along the segment.
#define FX_RIBBON_PIECE_ANGLE 0.13089969f  // pi / 24
float3 fxHermite(float3 p0, float3 m0, float3 p1, float3 m1, float s)
{
    const float s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * m0 + (3 * s2 - 2 * s3) * p1 + (s3 - s2) * m1;
}
float4 fxLookTexel(uint srv, uint frame, float2 uv, float2 cells, float2 border, float lod);
bool fxStripSampleOf(LayerConstants c, LayerExtra x, LayerRecord r, float2 p, float footprint, out float a, out float3 colour, out float depth, out uint blend,
                     out float2 motion)
{
    a = 0;
    colour = 0;
    depth = 0;
    blend = FX_BLEND_ALPHA;
    motion = 0;
    const uint k = r.radianceAlpha.x, j = r.radianceAlpha.y;
    RWStructuredBuffer<FxRibbonVertex> vertices = ResourceDescriptorHeap[c.ribbonVertices];  // (UAVs: written earlier in the pass)
    const FxRibbonVertex va0 = vertices[2u * j], vb0 = vertices[2u * k];
    const float3 a0 = va0.position, a1 = vertices[2u * j + 1u].position;
    const float3 b0 = vb0.position, b1 = vertices[2u * k + 1u].position;
    // the pixel-centre ray (view space at z = -1, then world axes; every projection here has no shear: mPixelRay)
    const float2 ndc = float2(p.x / g_viewWidth * 2 - 1, 1 - p.y / g_viewHeight * 2);
    const float vx = (ndc.x + g_proj[0][2] - g_proj[0][3]) / g_proj[0][0];
    const float vy = (ndc.y + g_proj[1][2] - g_proj[1][3]) / g_proj[1][1];
    const float3 D = g_view[0].xyz * vx + g_view[1].xyz * vy - g_view[2].xyz;  // view depth of D is 1
    float t, u, v, s = 0, e = 0;
    uint pieces = 1u;
    float3 ta = 0, tb = 0;
    const float3 pa = 0.5f * (a0 + a1), pb = 0.5f * (b0 + b1), sa = 0.5f * (a1 - a0), sb = 0.5f * (b1 - b0);
    const float len = length(pb - pa);
    if (x.ribbonSegments > 1u)
    {
        RWStructuredBuffer<float4> tangents = ResourceDescriptorHeap[x.ribbonTangents];
        ta = tangents[j].xyz * c.streamAxes;
        tb = tangents[k].xyz * c.streamAxes;
        pieces = clamp((uint)ceil(acos(clamp(dot(ta, tb), -1.0f, 1.0f)) / FX_RIBBON_PIECE_ANGLE), 1u, x.ribbonSegments);
    }
    bool hit = false;
    if (pieces == 1u)
    {
        if (fxRayTriangle(D, a0, a1, b0, t, u, v)) { s = v; e = u; hit = true; }
        else if (fxRayTriangle(D, b0, a1, b1, t, u, v)) { s = 1 - u; e = u + v; hit = true; }
    }
    else
    {
        [loop] for (uint i = 0u; i < pieces && !hit; ++i)
        {
            const float s0 = (float)i / pieces, s1 = (float)(i + 1u) / pieces;
            const float3 c0 = fxHermite(pa, ta * len, pb, tb * len, s0), c1 = fxHermite(pa, ta * len, pb, tb * len, s1);
            const float3 h0 = lerp(sa, sb, s0), h1 = lerp(sa, sb, s1);
            if (fxRayTriangle(D, c0 - h0, c0 + h0, c1 - h1, t, u, v)) { s = lerp(s0, s1, v); e = u; hit = true; }
            else if (fxRayTriangle(D, c1 - h1, c0 + h0, c1 + h1, t, u, v)) { s = lerp(s0, s1, 1 - u); e = u + v; hit = true; }
        }
    }
    if (!hit || !(t > g_nearPlane)) return false;
    RWStructuredBuffer<FxRibbonAppearance> appearance = ResourceDescriptorHeap[c.ribbonAppearance];
    const FxRibbonAppearance appA = appearance[j], appB = appearance[k];
    const float4 ca = lerp(fxUnpackHalf4(appA.radianceAlpha), fxUnpackHalf4(appB.radianceAlpha), saturate(s));
    motion = lerp(fxUnpackHalf2(appA.motion), fxUnpackHalf2(appB.motion), saturate(s));
    const float across = 2 * saturate(e) - 1;
    float4 tex = float4(1, 1, 1, (1 - across * across) * (1 - across * across));
    float shade = 1;
    const uint lookId = fxRecordLook(r);
    if (lookId != 0u)
    {
        StructuredBuffer<FxSpriteLook> looks = ResourceDescriptorHeap[x.looks];
        const FxSpriteLook look = looks[lookId - 1u];
        blend = fxLookBlend(look);
        const uint normalMode = fxLookNormal(look);
        float3 nt = float3(0, across, sqrt(saturate(1.0f - across * across)));  // (a tube's normal over the strip: along, across, out)
        if (look.texture != UNX_NONE)
        {
            StructuredBuffer<StreamProgram> programs = ResourceDescriptorHeap[c.programs];
            const StreamProgram pr = programs[r.program];
            float ua = va0.uv.x, ub = vb0.uv.x;
            if (fxLookRibbonUv(look) == 1u)
            {
                RWStructuredBuffer<FxRibbonPoint> points = ResourceDescriptorHeap[c.ribbonPoints];
                ua = points[j].age / max(pr.lifetime, 1e-6f);
                ub = points[k].age / max(pr.lifetime, 1e-6f);
            }
            const float2 cells = float2(max(pr.columns, 1u), max(pr.rows, 1u));
            const float2 scale = float2(pr.uv.x != 0 ? pr.uv.x : 1.0f, pr.uv.y != 0 ? pr.uv.y : 1.0f);
            const float2 uv = float2(lerp(ua, ub, saturate(s)), saturate(e)) * scale + pr.uv.zw;
            // the level whose texel covers the footprint: the texture's texels per pixel along and across the strip
            const float pixelWorld = 2.0f * t / (g_proj[1][1] * g_viewHeight);
            const float2 perPixel = look.textureSize / cells * abs(scale) * float2(abs(ub - ua), 1.0f) * pixelWorld / max(float2(len, 2.0f * length(lerp(sa, sb, saturate(s)))), 1e-6f);
            const float lod = max(log2(max(perPixel.x, perPixel.y) * footprint), 0.0f);
            const uint frame = min(pr.firstFrame, (uint)(cells.x * cells.y) - 1u);
            const float2 border = 0.5f * exp2(lod) * cells / max(look.textureSize, 1.0f);
            tex = fxLookTexel(look.texture, frame, uv, cells, border, lod);
            if (normalMode == FX_NORMAL_MAP)
            {
                const float2 n2 = fxLookTexel(look.normalTexture, frame, uv, cells, border, lod).rg * 2.0f - 1.0f;
                nt = float3(n2, sqrt(saturate(1.0f - dot(n2, n2))));
            }
        }
        if (normalMode != FX_NORMAL_NONE)
        {
            // the strip's frame at the hit: along it, across it (its side), and its normal on the viewer's side
            const float3 side = normalize(lerp(sa, sb, saturate(s)));
            float3 out3 = lerp(va0.normal, vb0.normal, saturate(s));
            out3 -= side * dot(out3, side);
            out3 = dot(out3, out3) > 1e-12f ? normalize(out3) : normalize(cross(side, pb - pa));
            if (dot(out3, D) > 0) out3 = -out3;
            const float3 along = cross(side, out3);
            const float3 n = normalize(along * nt.x + side * nt.y + out3 * nt.z);
            const float3 m = lerp(fxUnpackHalf4(appA.moment).xyz, fxUnpackHalf4(appB.moment).xyz, saturate(s));
            shade = max(0.0f, 1.0f + 2.0f * dot(m, n));
        }
    }
    a = saturate(ca.w * tex.a);
    colour = ca.rgb * tex.rgb * shade;
    if (blend == FX_BLEND_PREMULTIPLIED) colour *= ca.w;
    depth = g_nearPlane / t;
    if (c.airVolume != UNX_NONE && c.transmittance != UNX_NONE)
    {
        AtmosphereSrvs atm;
        atm.transmittance = c.transmittance;
        atm.multiScatter = c.multiScatter;
        atm.skyView = UNX_NONE;
        atm.aerial = c.airVolume;
        float3 inscatter, transmittance, sunAtDepth;
        atmosphereAirView(atm, p / float2(g_viewWidth, g_viewHeight), t, inscatter, transmittance, sunAtDepth);
        // (the air in front with the strip's coverage; an additive strip hides nothing and carries none)
        if (blend == FX_BLEND_ALPHA) colour = colour * transmittance + inscatter * g_exposure;
        else colour = colour * transmittance + (blend == FX_BLEND_PREMULTIPLIED ? inscatter * (g_exposure * a) : 0.0f);
    }
    return a > 0 || (blend == FX_BLEND_PREMULTIPLIED && any(colour > 0));
}
// (a straight strip without a look: the records the test reference draws)
bool fxStripSample(LayerConstants c, LayerRecord r, float2 p, out float a, out float3 colour, out float depth)
{
    uint blend;
    float2 motion;
    return fxStripSampleOf(c, (LayerExtra)0, r, p, 1.0f, a, colour, depth, blend, motion);
}

// ---- a look's sprite at a point
// One frame of a look's texture at uv (of the sprite: 0..1), at the level 'lod': a single image wraps (the program's uv
// scale and scroll), a flipbook's frame is read inside its cell, half a texel of the level away from the neighbours.
float4 fxLookTexel(uint srv, uint frame, float2 uv, float2 cells, float2 border, float lod)
{
    Texture2D<float4> t = ResourceDescriptorHeap[srv];
    if (cells.x * cells.y <= 1.0f) return t.SampleLevel(g_linearWrap, uv, lod);
    const float2 cell = float2(frame % (uint)cells.x, frame / (uint)cells.x);
    return t.SampleLevel(g_linearClamp, (cell + clamp(uv, border, 1.0f - border)) / cells, lod);
}
struct FxLookHit
{
    float a;        // opacity (alpha, premultiplied), or the weight of an additive sprite's colour
    float3 colour;  // radiance x exposure; premultiplied: already times its coverage
    float q;        // squared distance from the sprite's centre in half sizes (the ball of the soft particles)
    uint blend;     // FX_BLEND_*
};
// A look's sprite (fxRecordLook(r) != 0) at full-resolution point p: the point in the quad, the flipbook's two frames
// around the record's frame - each read where the motion-vector flipbook says its image is at that fraction (frame i
// displaced back by m_i t, frame i + 1 forward by m_(i+1) (1 - t): the usual flipbook motion-vector blend; the
// reference tree has only the plain two-frame lerp, SubUVLerp) - blended by the fraction, and the look's lighting by
// the pixel's normal (the sphere's over the quad, or the normal flipbook's; the record's moment is in the quad's
// frame). footprint: the spacing of the samples the caller interpolates (1: full resolution; FX_LAYER_SCALE: the
// layer): the textures are read at the level whose texel covers it.
bool fxLookSample(LayerConstants c, LayerExtra x, LayerRecord r, float2 p, float footprint, out FxLookHit h)
{
    h = (FxLookHit)0;
    StructuredBuffer<FxSpriteLook> looks = ResourceDescriptorHeap[x.looks];
    const FxSpriteLook look = looks[fxRecordLook(r) - 1u];
    const float4 axes = fxUnpackHalf4(r.axes);
    const float2 d = p - r.centre;
    const float det = axes.x * axes.w - axes.y * axes.z;
    if (abs(det) < 1e-12f) return false;
    const float2 s = float2(d.x * axes.w - d.y * axes.z, axes.x * d.y - axes.y * d.x) / det;  // right, up in half sizes
    if (any(abs(s) > 1.0f)) return false;
    const float4 ca = fxUnpackHalf4(r.radianceAlpha);
    h.q = dot(s, s);
    h.blend = fxLookBlend(look);
    float4 tex = float4(1, 1, 1, h.q < 1.0f ? (1.0f - h.q) * (1.0f - h.q) : 0.0f);  // (no texture: the round profile)
    float3 n = float3(s, sqrt(saturate(1.0f - h.q)));
    const uint normalMode = fxLookNormal(look);
    if (look.texture != UNX_NONE)
    {
        StructuredBuffer<StreamProgram> programs = ResourceDescriptorHeap[c.programs];
        const StreamProgram pr = programs[r.program];
        const float2 cells = float2(max(pr.columns, 1u), max(pr.rows, 1u));
        const uint frames = max(pr.columns, 1u) * max(pr.rows, 1u);
        const float2 scale = float2(pr.uv.x != 0 ? pr.uv.x : 1.0f, pr.uv.y != 0 ? pr.uv.y : 1.0f);
        const float2 uv = float2(0.5f + 0.5f * s.x, 0.5f - 0.5f * s.y) * scale + fxUnpackHalf2(r.uvOffset);
        const float2 texelsPerPixel = look.textureSize / cells * abs(scale) / (2.0f * max(float2(length(axes.xy), length(axes.zw)), 1e-6f));
        const float lod = max(log2(max(texelsPerPixel.x, texelsPerPixel.y) * footprint), 0.0f);
        const float2 border = 0.5f * exp2(lod) * cells / max(look.textureSize, 1.0f);
        const float f0 = floor(r.frame), t = (look.flags & FX_LOOK_FRAME_BLEND) != 0u ? r.frame - f0 : 0.0f;
        const uint i0 = (uint)f0 % frames;
        const uint i1 = (look.flags & FX_LOOK_FRAMES_OVER_LIFE) != 0u ? min(i0 + 1u, frames - 1u) : (i0 + 1u) % frames;
        float2 uv0 = uv, uv1 = uv;
        if (look.motionTexture != UNX_NONE && t > 0)
        {
            const float2 m0 = fxLookTexel(look.motionTexture, i0, uv, cells, border, lod).rg * 2.0f - 1.0f;
            const float2 m1 = fxLookTexel(look.motionTexture, i1, uv, cells, border, lod).rg * 2.0f - 1.0f;
            uv0 = uv - m0 * (look.motionScale * t);
            uv1 = uv + m1 * (look.motionScale * (1.0f - t));
        }
        tex = fxLookTexel(look.texture, i0, uv0, cells, border, lod);
        if (t > 0) tex = lerp(tex, fxLookTexel(look.texture, i1, uv1, cells, border, lod), t);
        if (normalMode == FX_NORMAL_MAP)
        {
            float2 nt = fxLookTexel(look.normalTexture, i0, uv0, cells, border, lod).rg;
            if (t > 0) nt = lerp(nt, fxLookTexel(look.normalTexture, i1, uv1, cells, border, lod).rg, t);
            nt = nt * 2.0f - 1.0f;
            n = float3(nt, sqrt(saturate(1.0f - dot(nt, nt))));
        }
    }
    const float shade = normalMode != FX_NORMAL_NONE ? max(0.0f, 1.0f + 2.0f * dot(fxRecordMoment(r), n)) : 1.0f;
    h.a = saturate(ca.w * tex.a);
    h.colour = ca.rgb * tex.rgb * shade;
    // (the air in front of the sprite: with its coverage; an additive look hides nothing and carries none)
    if (h.blend == FX_BLEND_PREMULTIPLIED) h.colour = h.colour * ca.w + fxRecordInscatter(r) * h.a;
    else h.colour += fxRecordInscatter(r);
    return h.a > 0 || (h.blend == FX_BLEND_PREMULTIPLIED && any(h.colour > 0));
}

// Opacity, premultiplied colour and device depth of a record without a look at full-resolution point p.
bool fxLayerSample(LayerConstants c, LayerRecord r, float2 p, out float a, out float3 colour, out float depth)
{
    if ((r.flags & FX_LAYER_RECORD_STRIP) != 0u) return fxStripSample(c, r, p, a, colour, depth);
    const float4 ca = fxUnpackHalf4(r.radianceAlpha);
    a = fxLayerOpacity(r, p, ca.w);
    colour = ca.rgb;
    depth = r.depth;
    return a > 0;
}
#endif
