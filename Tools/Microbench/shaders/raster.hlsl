// Mesh-shader raster floor. One meshlet = 8x8 vertex grid = 98 triangles (64 vertices), placed at a
// pseudo-random screen position and depth. Triangle "size" is the grid edge length in pixels.
// MODE (compile-time): 0 normal, 1 emit nothing (meshlet launch cost only), 2 cull odd triangles via SV_CullPrimitive,
//                      3 full-screen quad per meshlet (fill / early-z), 4 "1 triangle per meshlet"
// P[0].y triangle edge px (float bits), P[0].z width, P[0].w height (float bits)
// P[1].x layer count (mode 3), P[1].y depth order for mode 3 (0 front-to-back, 1 back-to-front), P[1].z seed, P[1].w dispatch x count

#ifndef MODE
#define MODE 0
#endif
#if MODE == 1
#define VCOUNT 0
#define PCOUNT 0
#elif MODE == 3
#define VCOUNT 4
#define PCOUNT 2
#elif MODE == 4
#define VCOUNT 3
#define PCOUNT 1
#else
#define VCOUNT 64
#define PCOUNT 98
#endif

struct V { float4 pos : SV_Position; };
struct Prim { uint id : ID; bool cull : SV_CullPrimitive; };

[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void MS(uint gtid : SV_GroupThreadID, uint3 gid3 : SV_GroupID,
        out vertices V verts[64], out indices uint3 tris[98], out primitives Prim prims[98])
{
    SetMeshOutputCounts(VCOUNT, PCOUNT);
#if MODE == 1
    if (P[1].x == 0xFFFFFFFFu) { verts[0].pos = 0; tris[0] = uint3(0, 0, 0); prims[0].id = 0; prims[0].cull = false; } // never executed; keeps the output signature intact
#endif
#if MODE != 1
    uint gid = gid3.x + gid3.y * P[1].w;
    float edge = asfloat(P[0].y);
    float W = asfloat(P[0].z), H = asfloat(P[0].w);
    uint seed = pcg(gid + P[1].z);
    float2 origin = float2(u01(seed) * W, u01(seed) * H);
    float z = 0.1 + 0.8 * u01(seed);
#if MODE == 3
    uint layers = max(P[1].x, 1u);
    z = (gid + 1) / (float)(layers + 1);
    if (P[1].y != 0) z = 1.0 - z;
#endif
    if (gtid < VCOUNT)
    {
        float2 p;
#if MODE == 3
        p = float2(gtid & 1, gtid >> 1) * float2(W, H) * 1.0001;
#elif MODE == 4
        p = origin + float2(gtid == 1 ? edge : 0, gtid == 2 ? edge : 0);
#else
        p = origin + float2(gtid & 7, gtid >> 3) * edge;
#endif
        verts[gtid].pos = float4(p.x / W * 2 - 1, 1 - p.y / H * 2, z, 1);
    }
    if (gtid < PCOUNT)
    {
        uint3 t;
        bool cull = false;
#if MODE == 3
        t = (gtid == 0) ? uint3(0, 1, 2) : uint3(2, 1, 3);
#elif MODE == 4
        t = uint3(0, 1, 2);
#else
        uint q = gtid >> 1, qx = q % 7, qy = q / 7, v0 = qy * 8 + qx;
        t = (gtid & 1) ? uint3(v0 + 1, v0 + 9, v0 + 8) : uint3(v0, v0 + 1, v0 + 8);
#if MODE == 2
        cull = (gtid & 1) != 0;
#endif
#endif
        tris[gtid] = t;
        prims[gtid].id = (gid << 7) | gtid;
        prims[gtid].cull = cull;
    }
#endif
}

uint PS(V v, nointerpolation uint id : ID) : SV_Target0
{
    return id;
}
