// Stream record layouts of the FX particle module (NativeVfxStream.h, StructuredBuffer 4-byte packing) and the stream's
// enums: shared by the tick kernels (Particles.hlsli) and the per-frame render kernels (ParticleLayerPass.hlsli), which
// bind different constants.
#ifndef FX_STREAM_RECORDS_HLSLI
#define FX_STREAM_RECORDS_HLSLI

#define FX_NONE 0xFFFFFFFFu

// Stream enums (NativeVfxStream.h)
#define FX_RESET 1u
#define FX_PROGRAM_NOISE 1u
#define FX_PROGRAM_WIND 2u
#define FX_PROGRAM_COLLISION 4u
#define FX_PROGRAM_COLLIDE_SELF 8u
#define FX_PROGRAM_COLLISION_EVENTS 16u
#define FX_PROGRAM_BIRTH_EVENTS 32u
#define FX_PROGRAM_DEATH_EVENTS 64u
#define FX_PROGRAM_ATTACHED 128u
#define FX_EMITTER_ACTIVE 1u
#define FX_EMITTER_KILLED 2u
#define FX_EMITTER_TRANSPORT 4u
#define FX_EMITTER_SOURCE 8u
#define FX_EMITTER_DEATH_EVENTS 16u
#define FX_EVENT_BIRTH 0u
#define FX_EVENT_DEATH 1u
#define FX_EVENT_COLLISION 2u
// Program outputs (NativeVfx.h NV_SPRITE..)
#define FX_OUTPUT_SPRITE 0u
#define FX_OUTPUT_MESH 1u
#define FX_OUTPUT_RIBBON 2u
#define FX_OUTPUT_VOLUME 3u
#define FX_OUTPUT_DECAL 4u
#define FX_OUTPUT_DISTORTION 5u
#define FX_OUTPUT_FLUID 6u

// ---- stream records (StructuredBuffer layouts: 4-byte packing, same order as NativeVfxStream.h) ------------------
struct StreamProgram  // 320 B
{
    uint output, material, flags, shape;
    float lifetime, drag, positionRadius, velocityRadius;
    float3 acceleration; float size;
    float3 velocity; float coneCos;
    float3 cone; float noiseFrequency;
    float3 box; float reserved0;
    float3 noise; float reserved1;
    float4 color;
    float restitution, friction, separation, reserved2;
    uint sizeKeys, sizeCount, colorKeys, colorCount;
    uint alphaKeys, alphaCount, rotationKeys, rotationCount;
    float4 uv;
    float2 uvScroll; float framesPerSecond, reserved3;
    uint columns, rows, firstFrame, mediumGrid;
    float refractionAmplitude, refractionWidth; uint refractionProfile, reserved4;
    float3 mediumAbsorption; float mediumPhase;
    float3 mediumScattering; float ribbonUv;
    float3 mediumEmission; float ribbonBreak;
    float3 ribbonNormal; float reserved5;
    uint4 reserved6;
};
struct StreamEmitter  // 336 B
{
    uint2 origin[3];                 // double world origin (renderer; the GPU never uses it)
    uint program, flags;
    float3 originAnchor; uint rngKey;
    float3 rebase; uint noiseKey;
    uint nextBirth, deathBirth, dyingBirth, deathEvent;
    float speed, sizeScale, dragVelocity, dragPosition;
    float dragAcceleration, reserved0, reserved1, reserved2;
    float4 colorScale;
    float3 inherited; uint parentEvent;
    float4 transport[3];
    float4 sourcePrevious[3];
    float4 sourceCurrent[3];
    float3 spawnOffset; uint parentRow;
    uint2 entity; uint2 generation;
    uint outputBase, reserved3, reserved4, reserved5;
};
struct StreamEmitterPatch  // 48 B (NV_StreamEmitterPatch): the per-tick fields of a row whose block the GPU holds
{
    uint row, flags, nextBirth, deathBirth;
    uint dyingBirth, deathEvent, outputBase, parentEvent;
    uint parentRow; float3 rebase;
};
struct StreamSpawn  // 48 B
{
    uint emitter, count, firstBirth, expired;
    uint kind, threadOffset, birthEvent, deathEvent;
    float interval, carry, rate, reserved0;
};
struct StreamExplicitBirth  // 48 B
{
    uint emitter, birth, birthEvent, deathEvent;
    float3 position; float elapsed;
    float3 velocity; float reserved0;
};
struct StreamField { float3 position; uint kind; float3 value; float radius; };  // 32 B
struct StreamWorldField { float3 origin; uint packed; float3 basis0; float3 basis1; float3 basis2; float3 value; };  // 64 B
struct StreamSurface  // 128 B (body == FX_NONE: anchor space; else a, b, c body-local)
{
    uint kind; uint2 entity; uint generation0;
    uint generation1; float radius; uint body, reserved1;
    float3 a; float reserved2; float3 b; float reserved3; float3 c; float reserved4;
    float3 velocity; float reserved5; float3 angular; float reserved6; float3 origin; float reserved7;
};
struct StreamBody { float4 rotation; float3 position; float reserved0; float3 velocity; float reserved1; float3 angular; float reserved2; float3 center; float reserved3; };  // 80 B
struct StreamParticle { uint emitter, birth, reserved0, reserved1; float3 position; float age; float3 velocity; float reserved2; };  // 48 B
struct StreamEvent { uint emitter, birth, kind, impacts; float3 position; float after; float3 velocity; float reserved0; float3 normal; float reserved1; };  // 64 B

// ---- module records ---------------------------------------------------------------------------------------------------
// Emitter values the GPU derives in the tick: every row starts from the table (FxBegin); a child row (parent_event)
// gets origin = float(parent origin_anchor + event position) and inherited = ratio x event velocity at its depth.
struct EmitterDynamic { float3 originAnchor; uint pad0; float3 inherited; uint pad1; };  // 32 B
#endif
