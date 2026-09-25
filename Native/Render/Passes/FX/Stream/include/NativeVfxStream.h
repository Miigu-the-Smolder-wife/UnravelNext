#ifndef UNRAVEL_NATIVE_VFX_STREAM_H
#define UNRAVEL_NATIVE_VFX_STREAM_H
/* CPU authority -> GPU particle module stream (WORLD_VFX_DESIGN_KO.md 3, V2).

   Owner: NativeVfx (Unravel). The UnravelNext particle module (FX track) keeps a
   byte-identical copy and checks its SHA-256 against this file when present.

   Division of authority
   - The CPU owns every discrete decision: which births exist (birth numbers),
     which particles die in a tick (a per-emitter birth-number prefix, exact in
     double on the CPU because a program's lifetime is constant), which events
     exist and where they are written (CPU-assigned event slots), every child
     emitter of a birth/death event (identity, row, counts, carry, elapsed), the
     alive count after the tick and the slot capacity.
   - The GPU owns particle state (position, velocity, age) and fills only the
     continuous quantities: states, event positions/velocities, collision events.
   - No GPU value decides life, death or identity. Float transcendental
     differences can therefore never change a count or an identity.

   Byte layout: little-endian, every struct is 16-byte aligned and uses only
   uint32/float except the renderer-facing double origin of an emitter and the
   double anchor/time of the header (the GPU never needs double arithmetic).
   A packet is one contiguous allocation: NV_StreamHeader followed by sections
   at the byte offsets it names (each 16-byte aligned; an absent section has
   count 0 and offset 0).

   Coordinates
   - The header's anchor is a double world point chosen by the CPU. Every
     world-space float in the packet (emitter origin_anchor, fields, world
     fields, surfaces) is relative to it.
   - A particle's position is relative to its emitter's origin (float3). The
     world position is origin (double) + position. A field or surface is
     evaluated at origin_anchor + position.
   - rebase: at the start of the tick every existing particle (age >= 0) of the
     emitter subtracts rebase from its position, because the CPU moved origin by
     exactly rebase (double; the float is exact up to rounding of the delta).

   One tick on the GPU (single queue, after the World commit of tick n)
   0. If RESET: every slot becomes dead, then the restore records are installed.
   1. Depth 0 spawn: generated births of spawn records [depth[0],depth[1]) and all
      explicit births. A birth writes its state at its birth instant with a
      negative age -elapsed (sign bit set, including -0.0), meaning "integrate
      for elapsed seconds this tick, starting at age 0". Births with rank <
      expired get no slot: they write their birth event (if any), integrate to
      their lifetime and write their death event (if any).
   2. Depth 0 integrate over every live slot (see NV_STREAM_* integrate order).
      A slot whose emitter is KILLED dies (no event). An existing slot first
      applies step a (rebase, transport); then, if its birth is in the
      emitter's dying range [dying_birth, death_birth) (mod 2^32), it dies: if the emitter has
      NV_STREAM_EMITTER_DEATH_EVENTS it is integrated for (lifetime - age)
      seconds and its death event is written at death_event + (birth -
      dying_birth); it is then removed. It is never integrated past its life.
   3. For depth d = 1..4: spawn records [depth[d],depth[d+1]) (children created by
      depth d-1 birth/death events; their emitter blocks have parent_event set),
      then integrate only the slots spawned at depth d.
   4. Compact. The alive count must equal header.alive_after; a mismatch sets
      NV_STREAM_STATUS_ALIVE_MISMATCH (a defect, never an expected state).
   5. Copy counters + event slots [0,event_slots) + collision events to the
      readback of tick n.
   A packet with dt == 0 is a state packet (restore, same-tick population
   change): only step 0, the KILLED rows and step 4 run; no spawn, no
   integration, tick unchanged. Without RESET it keeps the readback of its
   tick (events, collision events) and updates only counters.alive/status;
   with RESET the readback of the new generation's tick has no events.

   Integrate order for one slot with interval h (h = dt, or elapsed for a new
   birth, or lifetime - age for a death state), all in emitter-origin space:
     a. existing slot only: position -= rebase; if TRANSPORT: p = R p + t, v = R v
     b. acceleration = program.acceleration + noise + context fields (authored
        order) + world gravity (sampled at the start position)
     c. linear drag factors (vf, pf, af) for h: the emitter's precomputed full-dt
        factors when h == dt and the slot existed at tick start, else computed
        by NvLinearDrag(drag, h)
     d. p += v pf + a af; v = v vf + a pf; if drag != 0 and wind: p += w drag af,
        v += w drag pf (wind sampled at the start position)
     e. collision sweep start -> p against the surface list (at most four
        impacts; a fifth sets NV_STREAM_STATUS_IMPACT_OVERFLOW and the particle
        stays at its fourth contact point). The first impact of a slot whose
        program has NV_STREAM_PROGRAM_COLLISION_EVENTS appends one collision
        event (count = impacts of this interval).
     f. age += h
   The shared formulas are in shaders/VfxParticleMath.hlsli.

   Ribbon and volume outputs: the live births of one emitter are exactly the
   contiguous range [death_birth, next_birth) (mod 2^32) after the tick, so the
   output index of a live particle is output_base + (birth - death_birth).
   output_base counts ribbon points (NV_RIBBON) or medium cells (NV_VOLUME,
   grid^3 per particle); header.ribbon_points / medium_cells are the totals. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NV_STREAM_MAGIC UINT64_C(0x314d525453564e55) /* "UNVSTRM1" */
#define NV_STREAM_VERSION 1u
#define NV_STREAM_MAX_DEPTH 4u
#define NV_STREAM_NONE 0xffffffffu

enum {
    NV_STREAM_RESET=1u,        /* all slots dead, then restore records installed */
    NV_STREAM_PROGRAMS=2u,     /* program table + curve keys replace the previous ones */
    NV_STREAM_SURFACES=4u      /* surface table replaces the previous one (else the previous stays) */
};
/* Program flags. Kinematic outputs (beams, decals) never enter the stream. */
enum {
    NV_STREAM_PROGRAM_NOISE=1u,
    NV_STREAM_PROGRAM_WIND=2u,             /* drag != 0: world wind relaxes the velocity */
    NV_STREAM_PROGRAM_COLLISION=4u,
    NV_STREAM_PROGRAM_COLLIDE_SELF=8u,     /* else surfaces of the emitter's own entity lifetime are skipped */
    NV_STREAM_PROGRAM_COLLISION_EVENTS=16u,
    NV_STREAM_PROGRAM_BIRTH_EVENTS=32u,
    NV_STREAM_PROGRAM_DEATH_EVENTS=64u,
    NV_STREAM_PROGRAM_ATTACHED=128u        /* source-affine mode 1: particles transported by the source */
};
enum { NV_STREAM_SHAPE_POINT=0u,NV_STREAM_SHAPE_SPHERE=1u,NV_STREAM_SHAPE_BOX=2u,NV_STREAM_SHAPE_DISC=3u,NV_STREAM_SHAPE_EXPLICIT=4u };
/* Emitter flags. */
enum {
    NV_STREAM_EMITTER_ACTIVE=1u,       /* row in use this tick */
    NV_STREAM_EMITTER_KILLED=2u,       /* every slot of this row dies this tick (no events) */
    NV_STREAM_EMITTER_TRANSPORT=4u,    /* transport is valid (attached source moved) */
    NV_STREAM_EMITTER_SOURCE=8u,       /* births interpolate source_previous -> source_current */
    NV_STREAM_EMITTER_DEATH_EVENTS=16u /* dying slots write death events (program has death rules) */
};
enum { NV_STREAM_SPAWN_BURST=0u,NV_STREAM_SPAWN_SCHEDULED=1u };
enum { NV_STREAM_EVENT_BIRTH=0u,NV_STREAM_EVENT_DEATH=1u,NV_STREAM_EVENT_COLLISION=2u };
enum {
    NV_STREAM_STATUS_IMPACT_OVERFLOW=1u,  /* a particle needed a fifth impact in one interval */
    NV_STREAM_STATUS_NONFINITE=2u,        /* a non-finite state was produced (slot killed) */
    NV_STREAM_STATUS_ALIVE_MISMATCH=4u,   /* compaction count != alive_after (defect) */
    NV_STREAM_STATUS_CAPACITY=8u,         /* a spawn found no dead slot (defect: the CPU admits capacity) */
    NV_STREAM_STATUS_RANGE=16u            /* an index outside a range the packet declares (write skipped; defect) */
};

typedef struct NV_StreamHeader {
    uint64_t magic;            /* NV_STREAM_MAGIC */
    uint32_t version,flags;    /* NV_STREAM_VERSION, NV_STREAM_RESET | NV_STREAM_PROGRAMS */
    uint64_t bytes;            /* whole packet */
    uint64_t stream;           /* one VFX context: process-unique, never reused */
    uint64_t generation;       /* changes at every RESET; readbacks of another generation are stale */
    uint64_t tick;             /* World tick this packet advances to */
    double dt;                 /* tick interval (s) */
    double time;               /* context time at the end of this tick (s) */
    double anchor[3];          /* world anchor of every anchor-relative float */
    float dt_float,time_float; /* dt and time as float (time: for noise/visuals only) */
    uint32_t slot_capacity;    /* live slots never exceed this (CPU admitted) */
    uint32_t alive_after;      /* exact live count after step 4 */
    uint32_t program_count,curve_key_count,emitter_count,spawn_count;
    uint32_t explicit_count,field_count,world_field_count,surface_count;
    uint32_t restore_count;    /* RESET: records installed into slots 0..restore_count-1 */
    uint32_t event_slots;      /* CPU-assigned birth/death event slots of this tick */
    uint32_t collision_capacity; /* upper bound of collision events (live slots of colliding programs) */
    uint32_t ribbon_points,medium_cells; /* output totals (see output_base) */
    uint32_t depth[NV_STREAM_MAX_DEPTH+2]; /* spawn records of depth d: [depth[d], depth[d+1]) */
    uint32_t body_count;       /* rigid body frames of this tick (surfaces reference them) */
    uint64_t programs,curve_keys,emitters,spawns,explicit_births,fields,world_fields,surfaces,restore,bodies; /* byte offsets */
    uint64_t dynamic_surfaces; /* byte offset of this tick's world-space surfaces */
    uint32_t dynamic_surface_count,reserved0b;
    uint64_t reserved[5];
} NV_StreamHeader;

/* Static per program (index = program number). Curves are piecewise linear
   over normalized age u = age / lifetime in [0,1] with >= 2 keys, first t=0,
   last t=1, strictly increasing t. Evaluation: right = first key with t > u
   (the last key when none), left = right - 1, w = clamp((u - left.t) /
   (right.t - left.t), 0, 1), value = (1 - w) left + w right. Size keys are
   (t, multiplier), colour keys (t, r, g, b), alpha keys (t, a), rotation keys
   (t, radians); unused lanes are 0. A program without authored keys still has
   two keys (size: size-over-life begin/end; colour/alpha: 1). */
typedef struct NV_StreamProgram {
    uint32_t output,material,flags,shape;          /* NV_SPRITE.., NV_STREAM_PROGRAM_*, NV_STREAM_SHAPE_* */
    float lifetime,drag,position_radius,velocity_radius; /* position_radius: sphere radius or disc radius */
    float acceleration[3],size;                    /* constant acceleration (m/s^2), base size (m) */
    float velocity[3],cone_cos;                    /* constant birth velocity; cos(cone half angle) */
    float cone[3],noise_frequency;                 /* cone axis * speed (0: no cone) */
    float box[3],reserved0;                        /* box half extents */
    float noise[3],reserved1;                      /* noise acceleration amplitudes */
    float color[4];                                /* base colour (RGB HDR, alpha) */
    float restitution,friction,separation,reserved2;
    uint32_t size_keys,size_count,color_keys,color_count;       /* first key index, key count */
    uint32_t alpha_keys,alpha_count,rotation_keys,rotation_count; /* rotation_count 0: no rotation */
    float uv[4];                                   /* scale xy, offset zw */
    float uv_scroll[2],frames_per_second,reserved3;
    uint32_t columns,rows,first_frame,medium_grid;
    float refraction_amplitude,refraction_width;uint32_t refraction_profile,reserved4;
    float medium_absorption[3],medium_phase;
    float medium_scattering[3],ribbon_uv;
    float medium_emission[3],ribbon_break;         /* ribbon_break: FLT_MAX when unset */
    float ribbon_normal[3],reserved5;
    uint32_t reserved6[4];
} NV_StreamProgram;

/* One row of the dense emitter table (index = arena row; rows are stable for an
   emitter's lifetime and reused only after the row's last slot died). Birth
   numbers are the low 32 bits of the emitter's 64-bit counter; all interval
   comparisons are mod 2^32 (live spans are far below 2^31). */
typedef struct NV_StreamEmitter {
    double origin[3];            /* world origin (renderer) */
    uint32_t program,flags;      /* program index, NV_STREAM_EMITTER_* */
    float origin_anchor[3];      /* origin - anchor */
    uint32_t rng_key;            /* birth RNG key (CPU hash of seed, instance, asset) */
    float rebase[3];
    uint32_t noise_key;          /* noise key (CPU hash of instance and seed) */
    uint32_t next_birth,death_birth,dying_birth,death_event; /* live after tick: [death_birth,next_birth); dying: [dying_birth,death_birth) */
    float speed,size_scale;      /* gameplay parameters (birth velocity scale, size scale) */
    float drag_velocity,drag_position; /* NvLinearDrag(program.drag, dt) in double, rounded */
    float drag_acceleration,reserved0,reserved1,reserved2;
    float color_scale[4];        /* gameplay colour parameter (multiplies base colour) */
    float inherited[3];          /* velocity added to every birth (children: inherit * event velocity) */
    uint32_t parent_event;       /* child emitter created in this tick by a birth/death event (GPU chain):
                                    the event slot, else NV_STREAM_NONE. Then inherited.x is the inherit
                                    fraction (the GPU writes inherited = fraction * event velocity) and the
                                    GPU sets origin_anchor = parent origin_anchor + event position (float
                                    sum); the CPU defines the child's double origin as anchor +
                                    (double)that float, so later ticks send the identical value. */
    float transport[12];         /* attached source motion, row-major 3x4 [R|t] in origin space */
    float source_previous[12];   /* source map at tick start, [R|t - origin] */
    float source_current[12];    /* source map at tick end */
    float spawn_offset[3];       /* source-local spawn position (source programs), else 0 */
    uint32_t parent_row;         /* child emitter: parent row (for origin_anchor), else NV_STREAM_NONE */
    uint32_t entity[2],generation[2]; /* owner lifetime (self-collision exclusion) */
    uint32_t output_base,reserved3,reserved4,reserved5;
} NV_StreamEmitter;

/* A block of consecutive births of one emitter. Birth r (0 <= r < count) has
   number first_birth + r and elapsed (seconds from its birth to the tick end):
   BURST: elapsed = interval; SCHEDULED: elapsed = max(0, interval - (r + 1 -
   carry) / rate). Births with r < expired die inside this tick (no slot).
   thread_offset is the exclusive prefix of count within the record's depth, so
   the GPU finds a birth's record by binary search (no birth table upload).
   The source fraction of a birth is (dt - elapsed) / dt. */
typedef struct NV_StreamSpawn {
    uint32_t emitter,count,first_birth,expired;
    uint32_t kind,thread_offset,birth_event,death_event; /* event slots of rank 0 (NV_STREAM_NONE: none) */
    float interval,carry,rate,reserved0;
} NV_StreamSpawn;

/* A CPU-sampled birth (surface, skinned and morphed emission). */
typedef struct NV_StreamExplicitBirth {
    uint32_t emitter,birth,birth_event,death_event;
    float position[3],elapsed;   /* origin space at the birth instant */
    float velocity[3],reserved0;
} NV_StreamExplicitBirth;

typedef struct NV_StreamCurveKey {float t,value[3];} NV_StreamCurveKey;

/* NV_Field (spawn-context field) in anchor space. kind: NV_FIELD_ACCELERATION,
   NV_FIELD_RADIAL (a += d value.x / (r^2 sqrt(r^2)), d = position - p, r^2 =
   |d|^2 + radius^2), NV_FIELD_VORTEX (a += cross(value, (p - position) / dist),
   dist = sqrt(|p - position|^2 + radius^2)). */
typedef struct NV_StreamField {float position[3];uint32_t kind;float value[3],radius;} NV_StreamField;

/* NW_FieldRecord of quantity gravity (0) or wind (1), already filtered to this
   tick and in World priority order. packed = quantity | shape << 8 | operation
   << 16. local = inverse_basis (p - origin); inside: shape 0 always, 2 unit box
   |local|inf <= 1, 1 unit sphere (box first, then |local|^2 <= 1). Compose per
   axis in order: add, replace, min, max (min/max take the value when first). */
typedef struct NV_StreamWorldField {
    float origin[3];uint32_t packed;
    float inverse_basis[9];float value[3];
} NV_StreamWorldField;

/* Collision surface. kind 0 sphere (a, radius), 1 capsule (a, b, radius), 2
   double-sided triangle (a, b, c). body == NV_STREAM_NONE: a, b, c, velocity,
   angular_velocity and origin are anchor space (deformable surfaces written per
   tick). Otherwise a, b, c are body-local and the tick's body frame gives the
   world: p = R(q) p_local + position, surface velocity at a point x =
   velocity + cross(angular_velocity, x - center) with the body's centre of
   mass `center` (velocity/angular/origin of the record are then unused).
   Two sections: the static table (`surfaces`, replaced only with
   NV_STREAM_SURFACES, typically the body-local rigid surfaces) and this tick's
   dynamic world-space surfaces (`dynamic_surfaces`, deforming bodies, every
   tick). A surface's index for the collision tie rule is its static index, or
   surface_count + its dynamic index. Body frames come every tick. */
typedef struct NV_StreamSurface {
    uint32_t kind,entity[2],generation0;
    uint32_t generation1;float radius;uint32_t body,reserved1;
    float a[3],reserved2;float b[3],reserved3;float c[3],reserved4;
    float velocity[3],reserved5;float angular_velocity[3],reserved6;float origin[3],reserved7;
} NV_StreamSurface;
/* Rigid body frame of this tick (anchor space): unit quaternion (x, y, z, w),
   body origin, centre of mass, and the centre-of-mass velocities. */
typedef struct NV_StreamBody {
    float rotation[4];
    float position[3],reserved0;
    float velocity[3],reserved1;
    float angular_velocity[3],reserved2;
    float center[3],reserved3;
} NV_StreamBody;

/* A live slot: restore input (RESET) and checkpoint output. */
typedef struct NV_StreamParticle {
    uint32_t emitter,birth,reserved0,reserved1;
    float position[3],age;
    float velocity[3],reserved2;
} NV_StreamParticle;

/* Readback of one tick. Slot events [0,event_slots) are at CPU-assigned
   indices; collision events follow in any order (the CPU sorts them). */
typedef struct NV_StreamEvent {
    uint32_t emitter,birth,kind,impacts; /* impacts: collision count of the interval */
    float position[3],after;             /* origin space of the emitter; after: s from the event to tick end (collision) */
    float velocity[3],reserved0;         /* post-response velocity for collisions */
    float normal[3],reserved1;
} NV_StreamEvent;
typedef struct NV_StreamCounters {
    uint64_t stream,generation,tick;
    uint32_t alive,collision_events,status,reserved0;
    uint64_t reserved1;
} NV_StreamCounters;
typedef struct NV_StreamReadback {
    uint32_t size,version;
    NV_StreamCounters counters;
    const NV_StreamEvent* events;       /* event_slots + counters.collision_events records */
    uint64_t event_count;
} NV_StreamReadback;

/* Executor: the GPU module side. NativeVfx never sees a device.
   - submit: called by nv_commit with the committed packet (valid only during
     the call; copy it). It must not fail the commit: a device failure is
     recorded and reported by the next readback/checkpoint.
   - readback: the counters and events of stream/generation/tick. Called by the
     next tick's prepare; blocks until that tick executed. The pointers stay
     valid until the next call for this stream.
   - checkpoint: every live slot after tick (tick must be the stream's latest
     submitted tick). Explicit readback for save, views and CPU projection;
     never part of the per-tick path.
   - detach: the stream ends (context destroyed or another executor attached). */
typedef struct NV_StreamExecutor {
    uint32_t size,version;
    void* user;
    int32_t (*submit)(void* user,const uint8_t* packet,uint64_t bytes);
    int32_t (*readback)(void* user,uint64_t stream,uint64_t generation,uint64_t tick,NV_StreamReadback* output);
    int32_t (*checkpoint)(void* user,uint64_t stream,uint64_t generation,uint64_t tick,const NV_StreamParticle** records,uint64_t* count);
    void (*detach)(void* user,uint64_t stream);
} NV_StreamExecutor;

#ifdef __cplusplus
}
static_assert(sizeof(NV_StreamHeader)==320,"stream header");
static_assert(sizeof(NV_StreamProgram)==320,"stream program");
static_assert(sizeof(NV_StreamEmitter)==336,"stream emitter");
static_assert(sizeof(NV_StreamSpawn)==48&&sizeof(NV_StreamExplicitBirth)==48&&sizeof(NV_StreamCurveKey)==16,"stream births");
static_assert(sizeof(NV_StreamField)==32&&sizeof(NV_StreamWorldField)==64&&sizeof(NV_StreamSurface)==128&&sizeof(NV_StreamBody)==80,"stream inputs");
static_assert(sizeof(NV_StreamParticle)==48&&sizeof(NV_StreamEvent)==64&&sizeof(NV_StreamCounters)==48,"stream outputs");
#endif
#endif
