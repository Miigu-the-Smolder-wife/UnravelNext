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
   A state packet carries no tick inputs (fields, World fields, body frames,
   dynamic surfaces): the executor must not load or resolve them for it, since
   the static table's body rows would reference absent frames.

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
        stays at its fourth contact point). Each surface is swept in its own
        frame: a moving surface (velocity / angular_velocity about origin, the
        centre of mass, at the tick end) is tested against the particle's path
        relative to it over the interval h, so resting and struck particles neither
        tunnel through moving bodies nor start behind them (nv_collide). A
        candidate query must return every surface a relative path can meet. With
        theta = |w| h, u = |v| h, r a surface's extent from its centre of mass,
        D_c = u_c + theta_c (r_c + separation) for the carrier and d the query
        segment, a hit point x and the query point y at the same parameter obey
        |x - y| <= [theta r + (1 + theta) u] / (1 - theta)
                   + [(1 + theta) D_c + theta |d|] / (1 - theta):
        grow each surface by the first term and each query by the second (tick
        maxima are enough); a surface with theta >= 1/2 is always a candidate
        (bound derived by the FX V1 module). The sweep forms only small offsets
        relative to surfaces, so float error stays O(eps) of the magnitudes
        involved through a contact (tests/CollisionPrecision.h). The first impact of a slot whose
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
/* NV_StreamExecutor.version: 1, or 2 = the executor also executes the heightfield
   sections (height_fields, height_tiles; VfxParticleMath NV_HEIGHTFIELD hooks).
   A context whose World publishes heightfields refuses a version-1 executor
   (a terrain it cannot collide with is an error, not a silent pass-through). */
#define NV_STREAM_EXECUTOR_HEIGHTFIELDS 2u
/* 3 = also the wind turbulence of World wind fields (NW_WindTurbulence, B6/P12): a packet with
   NV_STREAM_WIND_TURBULENCE carries its world fields as NV_StreamWorldFieldTurbulent records (80 B) and the
   executor adds each record's curl-noise term with RuntimeCommon/WindField.hlsli's wfCurl (VfxParticleMath
   NV_WIND_TURBULENCE hook; shaders/VfxWindTurbulence.hlsli defines it for HLSL executors). A context whose World
   publishes turbulence refuses an executor below 3 (wind it cannot evaluate is an error, not a silent drop). */
#define NV_STREAM_EXECUTOR_WIND_TURBULENCE 3u
#define NV_STREAM_MAX_DEPTH 4u
#define NV_STREAM_NONE 0xffffffffu

enum {
    NV_STREAM_RESET=1u,        /* all slots dead, then restore records installed */
    NV_STREAM_PROGRAMS=2u,     /* program table + curve keys replace the previous ones */
    NV_STREAM_SURFACES=4u,     /* surface table replaces the previous one (else the previous stays) */
    NV_STREAM_WIND_TURBULENCE=16u, /* world_fields are NV_StreamWorldFieldTurbulent (executor version >= 3) */
    NV_STREAM_EMITTER_DELTA=8u /* emitters[] updates only the rows listed in emitter_rows[] of the
                                  persistent table (emitter_table rows); without it emitters[] is the
                                  whole table (emitter_count == emitter_table). A row not sent keeps its
                                  block, except the per-tick fields, which read as absent: rebase = 0,
                                  no TRANSPORT / SOURCE / KILLED flag, parent_event = parent_row = NONE.
                                  Its [dying_birth, death_birth) range was already applied (no live
                                  birth is below death_birth), so it kills nothing and writes no event.
                                  The CPU sends a whole table on RESET, the first packet, a new dt
                                  (drag factors) and a new anchor (WORLD_VFX_DESIGN_KO.md 9.6).
                                  With it, emitter_patches[] (NV_StreamEmitterPatch) update other
                                  listed rows' per-tick fields only: a row gets at most one of a
                                  block, a patch or nothing in a packet. A patch writes flags,
                                  next/death/dying births, death_event, output_base, parent_event,
                                  parent_row and rebase of the row the executor holds (the per-tick
                                  ones are this tick's, as in a block); every other field keeps the
                                  held block. The CPU sends a patch only when the held block with the
                                  patch applied equals the block it would send (except `origin`, which
                                  no executor reads), so a patch and a block give the same table. TRANSPORT and SOURCE never appear in a
                                  patch (those rows send blocks). */
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
    uint32_t dynamic_surface_count;
    uint32_t emitter_table;    /* rows of the persistent emitter table after this packet */
    uint64_t emitter_rows;     /* NV_STREAM_EMITTER_DELTA: byte offset of uint32 rows[emitter_count]
                                  (ascending, section padded to 16 B), row of each emitters[] block */
    uint64_t emitter_patches;  /* NV_STREAM_EMITTER_DELTA: byte offset of NV_StreamEmitterPatch[emitter_patch_count]
                                  (rows ascending, none also in emitter_rows) */
    uint32_t emitter_patch_count;
    uint32_t height_field_count; /* heightfields of the surface table (sent with it: NV_STREAM_SURFACES / RESET);
                                    0 for version-1 executors (formerly reserved) */
    uint64_t height_fields;      /* byte offset of NV_StreamHeightField[height_field_count] */
    uint64_t height_tiles;       /* byte offset of NV_StreamHeightTile[sum of tiles_x tiles_z] */
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
    double origin[3];            /* world origin, for CPU-side consumers only: executors never read it, and
                                    their copy may lag for chain-child rows (a patch leaves it; renderers
                                    form world positions as anchor + origin_anchor + position) */
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
                                    sum), where the parent's origin_anchor is its origin of this tick: the
                                    table value, or for a parent itself created in this tick (depth >= 2)
                                    the value the GPU resolved at the previous depth. The CPU defines the
                                    child's double origin as anchor + (double)that float, so later ticks
                                    send the identical value. After the tick the executor keeps both
                                    resolved values in the row (origin_anchor = that float sum,
                                    inherited = fraction x event velocity, float product, all three
                                    components): a later packet that sends no block for the row reads
                                    them. */
    float transport[12];         /* attached source motion, row-major 3x4 [R|t] in origin space */
    float source_previous[12];   /* source map at tick start, [R|t - origin] */
    float source_current[12];    /* source map at tick end */
    float spawn_offset[3];       /* source-local spawn position (source programs), else 0 */
    uint32_t parent_row;         /* child emitter: parent row (for origin_anchor), else NV_STREAM_NONE */
    uint32_t entity[2],generation[2]; /* owner lifetime (self-collision exclusion) */
    uint32_t output_base,reserved3,reserved4,reserved5;
} NV_StreamEmitter;
/* Per-tick fields of a row whose block the executor already holds (NV_STREAM_EMITTER_DELTA). */
typedef struct NV_StreamEmitterPatch {
    uint32_t row,flags;          /* flags: the row's whole flags word this tick (never TRANSPORT or SOURCE) */
    uint32_t next_birth,death_birth,dying_birth,death_event;
    uint32_t output_base,parent_event,parent_row;
    float rebase[3];
} NV_StreamEmitterPatch;

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
/* With NV_STREAM_WIND_TURBULENCE: the record plus its wind turbulence (rms 0 = none), prepared on the CPU with
   the World sampler's float operations (RuntimeCommon/WindTurbulence.h add_turbulence): the term added to value
   before the operation is wfCurl(d * inv_length, phase, octaves_seed & 255, octaves_seed >> 8) * rms, d = the
   sampled position minus origin, inv_length = 1 / length_m (float), phase = float(seconds) / period_s (float),
   seconds = the packet's World time (tick x dt, the host's fixed step). */
typedef struct NV_StreamWorldFieldTurbulent {
    NV_StreamWorldField field;
    float rms,inv_length,phase;uint32_t octaves_seed;
} NV_StreamWorldFieldTurbulent;

/* Collision surface. kind 0 sphere (a, radius), 1 capsule (a, b, radius), 2
   double-sided triangle (a, b, c). body == NV_STREAM_NONE: origin is an anchor-
   space reference point of the surface (its motion centre when it moves, else an
   interior point such as the centroid) and a, b, c are offsets from it; velocity
   and angular_velocity are about origin (deformable surfaces written per tick).
   Otherwise a, b, c are body-local and the tick's body frame gives the tick
   surface: origin = the body's centre of mass `center`, offsets R(q) p_local +
   (position - center), surface velocity at a point x = velocity +
   cross(angular_velocity, x - center) (velocity/angular/origin of the record are
   then unused). Geometry is thus only small offsets from a nearby point: its
   float precision is eps x surface size, not eps x distance from the anchor.
   Two sections: the static table (`surfaces`, replaced only with
   NV_STREAM_SURFACES, typically the body-local rigid surfaces) and this tick's
   dynamic world-space surfaces (`dynamic_surfaces`, deforming bodies, every
   tick). A surface's index for the collision tie rule is its static index, or
   surface_count + its dynamic index. Body frames come with every simulating
   packet (dt > 0); state packets carry none. */
typedef struct NV_StreamSurface {
    uint32_t kind,entity[2],generation0;
    uint32_t generation1;float radius;uint32_t body,reserved1;
    float a[3],reserved2;float b[3],reserved3;float c[3],reserved4;
    float velocity[3],reserved5;float angular_velocity[3],reserved6;float origin[3],reserved7;
} NV_StreamSurface;
/* Static heightfield (a terrain body, World NW_COMPONENT_BODY_HEIGHT_TILE), part
   of the surface table. Body frame: sample (x, z) at origin + (x spacing_x,
   h, z spacing_z), h = the sample's height; cell (x, z) splits along
   (x, z)-(x + 1, z + 1) into triangle 0 {(x,z),(x+1,z+1),(x+1,z)} and 1
   {(x,z),(x,z+1),(x+1,z+1)}, each colliding only on its upward (+y body axis)
   face. Its tiles are tiles_x x tiles_z consecutive NV_StreamHeightTile records
   from first_tile, row-major (tile (tx, tz) = cells [16 tx, 16 tx + 16) x
   [16 tz, 16 tz + 16)). `body` indexes the tick's body frames (the body must be
   static: zero velocities). For the collision tie rule, heightfield triangle
   (cell (cx, cz), t) has index surface_count + dynamic_surface_count +
   first_triangle + (cz cells_x + cx) 2 + t, first_triangle = sum of
   2 cells_x cells_z over the earlier heightfields. */
typedef struct NV_StreamHeightField {
    uint32_t body,entity[2],generation0;
    uint32_t generation1,cells_x,cells_z,first_tile;
    uint32_t tiles_x,tiles_z,first_triangle,reserved0;
    float origin[3],spacing_x;   /* body frame */
    float spacing_z,reserved1,reserved2,reserved3;
} NV_StreamHeightField;
/* 16 x 16 cells of a heightfield: holes bit (z 16 + x) 2 + t set = that
   triangle does not exist; heights of the 17 x 17 samples [z 17 + x] (samples
   past the heightfield are 0 and their triangles holes). */
typedef struct NV_StreamHeightTile {
    uint32_t holes[16];
    float heights[17*17];
    float reserved[3];
} NV_StreamHeightTile;
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
static_assert(sizeof(NV_StreamEmitterPatch)==48,"stream emitter patch");
static_assert(sizeof(NV_StreamSpawn)==48&&sizeof(NV_StreamExplicitBirth)==48&&sizeof(NV_StreamCurveKey)==16,"stream births");
static_assert(sizeof(NV_StreamField)==32&&sizeof(NV_StreamWorldField)==64&&sizeof(NV_StreamWorldFieldTurbulent)==80&&sizeof(NV_StreamSurface)==128&&sizeof(NV_StreamBody)==80,"stream inputs");
static_assert(sizeof(NV_StreamHeightField)==80&&sizeof(NV_StreamHeightTile)==1232,"stream heightfields");
static_assert(sizeof(NV_StreamParticle)==48&&sizeof(NV_StreamEvent)==64&&sizeof(NV_StreamCounters)==48,"stream outputs");
#endif
#endif
