// Shared particle mathematics of the CPU->GPU stream (include/NativeVfxStream.h).
// Owner: NativeVfx (Unravel). The UnravelNext particle module keeps a
// byte-identical copy (SHA-256 checked when the original is present).
//
// One source for both executors:
// - HLSL (GPU module): nv_real = float. Only 32-bit integer and float
//   operations; no int64, no double, no dynamically indexed local arrays.
// - C++ (NativeVfx CPU reference executor): include this file INSIDE a struct
//   body so that the buffer hooks below become member accesses; nv_real is
//   NV_REAL (double unless defined otherwise) and the vector types come from
//   the shim the includer defines (see src/VfxStreamCpu.h).
//
// Include twice: first with NV_PARTICLE_MATH_TYPES_ONLY defined (types), then
// define the hooks (they return the types below), then include again (functions).
// The includer defines these hooks before the second include:
//   NV_FIELD_COUNT, NV_FIELD(i)             -> NvField   (context fields, anchor space)
//   NV_WORLD_FIELD_COUNT, NV_WORLD_FIELD(i) -> NvWorldField
//   NV_SURFACE_COUNT, NV_SURFACE(i)         -> NvSurface
//   NV_CURVE_KEY(i)                         -> nv_real4 (t, v0, v1, v2)
//   optional NV_SURFACE_QUERY_TYPE / NV_SURFACE_QUERY / NV_SURFACE_NEXT (see nv_collide)
//
// Rule for this file: at most one call that changes an inout state per
// expression (C++ and HLSL evaluate function arguments in different orders).
//
// Nothing here decides life, death, identity or counts (the CPU does); the
// results are continuous states only, so transcendental differences between
// devices change values by float rounding, never a discrete outcome.

#ifndef NV_PARTICLE_MATH_TYPES
#define NV_PARTICLE_MATH_TYPES

#ifdef __cplusplus
#define NV_INOUT(T) T&
#define NV_OUT(T) T&
#define NV_LOOP
#define NV_UNROLL
#else
#define NV_LOOP [loop]
#define NV_UNROLL [unroll]
typedef float nv_real;
typedef float2 nv_real2;
typedef float3 nv_real3;
typedef float4 nv_real4;
#define NV_INOUT(T) inout T
#define NV_OUT(T) out T
#define nv_make3(x,y,z) float3(x,y,z)
#define nv_make4(x,y,z,w) float4(x,y,z,w)
#endif
#define NV_R(x) ((nv_real)(x))

// Inputs in the form the formulas need (filled from the stream records).
struct NvField { nv_real3 position; uint kind; nv_real3 value; nv_real radius; };
struct NvWorldField { nv_real3 origin; uint quantity; uint shape; uint operation; nv_real3 basis0; nv_real3 basis1; nv_real3 basis2; nv_real3 value; };
struct NvSurface { uint kind; uint entity0; uint entity1; uint generation0; uint generation1; nv_real radius; nv_real3 a; nv_real3 b; nv_real3 c; nv_real3 velocity; nv_real3 angular; nv_real3 origin; };
struct NvState { nv_real3 position; nv_real3 velocity; nv_real age; };
struct NvDrag { nv_real velocity; nv_real position; nv_real acceleration; };
struct NvImpact { uint count; nv_real3 contact; nv_real3 velocity; nv_real3 normal; nv_real fraction; };
// Per-slot integration parameters (emitter + program of the slot).
struct NvMotion {
    nv_real3 acceleration;   // program constant acceleration
    nv_real drag;
    nv_real3 noise;          // noise amplitudes (0: none)
    nv_real noise_frequency;
    uint noise_seed;         // nv_noise_seed(emitter.noise_key, birth)
    uint wind;               // 1: world wind relaxes the velocity (drag != 0)
    uint collision;          // 1: collide with surfaces
    uint self;               // 1: also collide with the owner's own surfaces
    uint entity0; uint entity1; uint generation0; uint generation1;
    nv_real restitution; nv_real friction; nv_real separation;
    nv_real3 origin_anchor;  // emitter origin - anchor (fields and surfaces are anchor space)
};
#endif // NV_PARTICLE_MATH_TYPES

#if !defined(NV_PARTICLE_MATH_TYPES_ONLY) && !defined(NV_PARTICLE_MATH_FUNCTIONS)
#define NV_PARTICLE_MATH_FUNCTIONS

// ---- integers and random numbers ---------------------------------------------------------
uint nv_hash(uint v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; return v ^ (v >> 16); }
// Noise seed of one particle: the CPU hashes the emitter instance and seed into noise_key.
uint nv_noise_seed(uint noise_key, uint birth) { return noise_key ^ nv_hash(birth); }
// Birth RNG: counter based, keyed by the emitter and the birth number only (never by slot or order).
uint nv_birth_rng(uint rng_key, uint birth) { return nv_hash(rng_key ^ nv_hash(birth + 0x9e3779b9u)); }
nv_real nv_next01(NV_INOUT(uint) state) { state = nv_hash(state + 0x9e3779b9u); return NV_R(state >> 8) * NV_R(1.0 / 16777216.0); }

// ---- linear drag (dv/dt = a - k v, constant a) --------------------------------------------
// Series for small k h, closed form otherwise; the CPU passes the full-dt
// factors computed in double, this is used for partial intervals.
NvDrag nv_linear_drag(nv_real k, nv_real h) {
    NvDrag d;
    if (k == NV_R(0)) { d.velocity = NV_R(1); d.position = h; d.acceleration = (NV_R(0.5) * h) * h; return d; }
    nv_real x = k * h;
    if (x <= NV_R(0.5)) {
        // phi2(-x) = sum (-x)^n / (n+2)!, degree 12 (truncation < 1e-12 at x = 0.5)
        nv_real p = NV_R(1.0 / 87178291200.0);
        p = NV_R(1.0 / 6227020800.0) - x * p;
        p = NV_R(1.0 / 479001600.0) - x * p;
        p = NV_R(1.0 / 39916800.0) - x * p;
        p = NV_R(1.0 / 3628800.0) - x * p;
        p = NV_R(1.0 / 362880.0) - x * p;
        p = NV_R(1.0 / 40320.0) - x * p;
        p = NV_R(1.0 / 5040.0) - x * p;
        p = NV_R(1.0 / 720.0) - x * p;
        p = NV_R(1.0 / 120.0) - x * p;
        p = NV_R(1.0 / 24.0) - x * p;
        p = NV_R(1.0 / 6.0) - x * p;
        p = NV_R(0.5) - x * p;
        nv_real phi1 = NV_R(1) - x * p;
        d.velocity = NV_R(1) - x * phi1; d.position = h * phi1; d.acceleration = (h * p) * h;
        return d;
    }
    nv_real e = exp(-x), q = (NV_R(1) - e) / k;
    d.velocity = e; d.position = q; d.acceleration = (h - q) / k;
    return d;
}

// ---- curves --------------------------------------------------------------------------------
// Piecewise linear over u in [0,1]; keys [first, first+count), count >= 2.
nv_real4 nv_curve(uint first, uint count, nv_real u) {
    uint right = first + 1;
    NV_LOOP while (right + 1 < first + count && !(NV_CURVE_KEY(right).x > u)) ++right;
    // right is now the first key with t > u, or the last key
    nv_real4 a = NV_CURVE_KEY(right - 1), b = NV_CURVE_KEY(right);
    nv_real w = clamp((u - a.x) / (b.x - a.x), NV_R(0), NV_R(1));
    return nv_make4(a.x, (NV_R(1) - w) * a.y + w * b.y, (NV_R(1) - w) * a.z + w * b.z, (NV_R(1) - w) * a.w + w * b.w);
}

// ---- noise ---------------------------------------------------------------------------------
nv_real nv_noise_value(uint seed, uint cell, uint axis) {
    return NV_R(nv_hash(seed ^ nv_hash(cell) ^ ((axis + 1u) * 0x9e3779b9u)) & 0xffffffu) * NV_R(1.0 / 8388608.0) - NV_R(1);
}
nv_real nv_smooth_noise(uint seed, uint axis, nv_real time) {
    uint cell = (uint)time; nv_real t = time - NV_R(cell), u = t * t * (NV_R(3) - NV_R(2) * t);
    return (NV_R(1) - u) * nv_noise_value(seed, cell, axis) + u * nv_noise_value(seed, cell + 1u, axis);
}

// ---- fields ----------------------------------------------------------------------------------
// q: anchor-space point. Context fields in authored order.
nv_real3 nv_context_fields(nv_real3 q) {
    nv_real3 a = nv_make3(NV_R(0), NV_R(0), NV_R(0));
    NV_LOOP for (uint f = 0u; f < NV_FIELD_COUNT; ++f) {
        NvField field = NV_FIELD(f);
        if (field.kind == 0u) a = a + field.value;
        else if (field.kind == 2u) {
            nv_real3 d = q - field.position;
            nv_real dist = sqrt(dot(d, d) + field.radius * field.radius);
            d = d * (NV_R(1) / dist);
            a = a + cross(field.value, d);
        } else {
            nv_real3 d = field.position - q;
            nv_real r2 = dot(d, d) + field.radius * field.radius;
            a = a + d * (field.value.x / (r2 * sqrt(r2)));
        }
    }
    return a;
}
// World field sample of quantity 0 (gravity) or 1 (wind) at anchor-space q.
nv_real3 nv_world_field(uint quantity, nv_real3 q) {
    nv_real3 result = nv_make3(NV_R(0), NV_R(0), NV_R(0)); uint contributors = 0u;
    NV_LOOP for (uint f = 0u; f < NV_WORLD_FIELD_COUNT; ++f) {
        NvWorldField field = NV_WORLD_FIELD(f);
        if (field.quantity != quantity) continue;
        if (field.shape != 0u) {
            nv_real3 delta = q - field.origin;
            nv_real3 l = nv_make3(dot(field.basis0, delta), dot(field.basis1, delta), dot(field.basis2, delta));
            if (abs(l.x) > NV_R(1) || abs(l.y) > NV_R(1) || abs(l.z) > NV_R(1)) continue;
            if (field.shape == 1u && dot(l, l) > NV_R(1)) continue;
        }
        bool first = contributors == 0u;
        if (field.operation == 0u) result = result + field.value;
        else if (field.operation == 1u || first) result = field.value;
        else if (field.operation == 2u) result = min(result, field.value);
        else result = max(result, field.value);
        ++contributors;
    }
    return result;
}

// ---- collision (visual point sweeps) -------------------------------------------------------
// Candidate enumeration hook. Default: every surface in order. An includer may
// define NV_SURFACE_QUERY_TYPE, NV_SURFACE_QUERY(p, d) and NV_SURFACE_NEXT(q, n)
// to enumerate a superset of the surfaces the segment p -> p + d can hit.
#ifndef NV_SURFACE_QUERY
struct NvSurfaceQuery { uint next; };
NvSurfaceQuery nv_surface_query_all(nv_real3 p, nv_real3 d) { NvSurfaceQuery q; q.next = 0u; return q; }
bool nv_surface_next_all(NV_INOUT(NvSurfaceQuery) q, NV_OUT(uint) n) {
    if (q.next >= NV_SURFACE_COUNT) { n = 0u; return false; }
    n = q.next; q.next = q.next + 1u; return true;
}
#define NV_SURFACE_QUERY_TYPE NvSurfaceQuery
#define NV_SURFACE_QUERY(p, d) nv_surface_query_all(p, d)
#define NV_SURFACE_NEXT(q, n) nv_surface_next_all(q, n)
#endif
// Entry root of a t^2 + 2 b t + c = 0 (the smaller one) without cancellation:
// for an approaching segment (b < 0) c / (-b + sqrt(h)), else (-b - sqrt(h)) / a.
// Contacts start close to surfaces (c near 0): the textbook form cancels there.
nv_real nv_entry_root(nv_real a, nv_real b, nv_real c, nv_real h) {
    nv_real root = sqrt(h);
    return b < NV_R(0) ? c / (root - b) : (-b - root) / a;
}
// Geometry is formed relative to the surface (p - center first): positions are
// emitter-origin space and far points would round the small offsets away.
// The segment starts at p + offset (offset: a small shift such as a surface's carry).
bool nv_hit_sphere(nv_real3 p, nv_real3 offset, nv_real3 d, nv_real3 center, nv_real radius, NV_OUT(nv_real) t, NV_OUT(nv_real3) normal) {
    t = NV_R(0); normal = nv_make3(NV_R(0), NV_R(0), NV_R(0));
    nv_real3 delta = (p - center) + offset;
    nv_real a = dot(d, d), b = dot(delta, d), c = dot(delta, delta) - radius * radius, h = b * b - a * c;
    if (a <= NV_R(0) || h < NV_R(0)) return false;
    nv_real candidate = nv_entry_root(a, b, c, h);
    if (!(candidate >= NV_R(0) && candidate <= NV_R(1))) return false;
    normal = delta + d * candidate; nv_real size = length(normal);
    if (!(size > NV_R(0))) return false;
    t = candidate; normal = normal * (NV_R(1) / size); return true;
}
// p is emitter-origin space; the surface's a, b, c are offsets from s.origin.
bool nv_hit(NvSurface s, nv_real3 p, nv_real3 offset, nv_real3 d, NV_OUT(nv_real) t, NV_OUT(nv_real3) normal) {
    t = NV_R(0); normal = nv_make3(NV_R(0), NV_R(0), NV_R(0));
    p = p - s.origin;   // the segment start relative to the surface's reference point
    if (s.kind == 0u) return nv_hit_sphere(p, offset, d, s.a, s.radius, t, normal);
    if (s.kind == 2u) {
        nv_real3 e1 = s.b - s.a, e2 = s.c - s.a, h = cross(d, e2);
        nv_real determinant = dot(e1, h); if (determinant == NV_R(0)) return false;
        nv_real3 delta = (p - s.a) + offset, q = cross(delta, e1);
        nv_real u = dot(delta, h) / determinant, v = dot(d, q) / determinant, candidate = dot(e2, q) / determinant;
        if (!(candidate >= NV_R(0) && candidate <= NV_R(1)) || u < NV_R(0) || v < NV_R(0) || u + v > NV_R(1)) return false;
        normal = cross(e1, e2); nv_real size = length(normal);
        if (!(size > NV_R(0))) return false;
        normal = normal * ((dot(normal, d) > NV_R(0) ? NV_R(-1) : NV_R(1)) / size); t = candidate; return true;
    }
    nv_real3 axis = s.b - s.a, delta = (p - s.a) + offset;
    nv_real aa = dot(axis, axis), ad = dot(axis, d), ao = dot(axis, delta), dd = dot(d, d), od = dot(delta, d), oo = dot(delta, delta), radius = s.radius;
    if (aa <= NV_R(0)) return nv_hit_sphere(p, offset, d, s.a, radius, t, normal);
    nv_real qa = aa * dd - ad * ad, qb = aa * od - ao * ad, qc = aa * oo - ao * ao - radius * radius * aa, hh = qb * qb - qa * qc;
    bool found = false; t = NV_R(2);
    if (qa > NV_R(0) && hh >= NV_R(0)) {
        nv_real candidate = nv_entry_root(qa, qb, qc, hh), y = ao + candidate * ad;
        if (candidate >= NV_R(0) && candidate <= NV_R(1) && y >= NV_R(0) && y <= aa) {
            nv_real3 n = delta + d * candidate - axis * (y / aa); nv_real size = length(n);
            if (size > NV_R(0)) { t = candidate; normal = n * (NV_R(1) / size); found = true; }
        }
    }
    NV_UNROLL for (uint end = 0u; end < 2u; ++end) {
        nv_real candidate; nv_real3 n;
        if (nv_hit_sphere(p, offset, d, end != 0u ? s.b : s.a, radius, candidate, n)) {
            nv_real y = ao + candidate * ad;
            if ((end != 0u ? y >= aa : y <= NV_R(0)) && candidate < t) { t = candidate; normal = n; found = true; }
        }
    }
    return found;
}
nv_real3 nv_bounce(nv_real3 value, nv_real3 normal, nv_real restitution, nv_real friction) {
    nv_real incoming = dot(value, normal);
    return incoming < NV_R(0) ? (value - normal * incoming) * (NV_R(1) - friction) - normal * (incoming * restitution) : value;
}
// Rigid motion of a surface over its last tau seconds (constant velocity v and
// angular velocity w about its centre of mass `origin`, both given at the tick end).
// Only displacements are formed (never an absolute carried point): positions are
// emitter-origin space and can be far from the origin, so an absolute point
// followed by a difference would cancel in float.
// (R(w) - I) x with R the rotation by |w| about w: a (w x x) + b w x (w x x),
// a = sin(t)/t, b = (1 - cos t)/t^2 = 2 (sin(t/2)/t)^2 (no 1 - cos cancellation).
nv_real3 nv_rotation_delta(nv_real3 w, nv_real3 x) {
    nv_real angle = length(w);
    if (angle == NV_R(0)) return nv_make3(NV_R(0), NV_R(0), NV_R(0));
    nv_real a = sin(angle) / angle, half = sin(angle * NV_R(0.5)) / angle, b = NV_R(2) * half * half;
    nv_real3 wx = cross(w, x);
    return wx * a + cross(w, wx) * b;
}
// carry(p) - p: where a point that moved with the surface for tau seconds ends,
// relative to where it was. carry(p) = origin + R(p - origin + v tau).
nv_real3 nv_surface_carry_delta(NvSurface s, nv_real tau, nv_real3 p) {
    nv_real3 step = s.velocity * tau;
    return nv_rotation_delta(s.angular * tau, p - s.origin + step) + step;
}
// uncarry(q) - q: where a point moving with the surface was tau seconds before the tick end.
nv_real3 nv_surface_uncarry_delta(NvSurface s, nv_real tau, nv_real3 q) {
    return nv_rotation_delta(s.angular * (-tau), q - s.origin) - s.velocity * tau;
}
bool nv_surface_moves(NvSurface s) {
    return s.velocity.x != NV_R(0) || s.velocity.y != NV_R(0) || s.velocity.z != NV_R(0) ||
           s.angular.x != NV_R(0) || s.angular.y != NV_R(0) || s.angular.z != NV_R(0);
}
// Sweeps origin-space start -> start + move over the interval h, each surface in
// its own frame: the particle's path relative to a moving surface is tested
// against the surface's tick-end geometry (tick-end coordinates are the world
// frame), so a particle resting on or struck by a moving body neither tunnels
// nor starts behind it. After a bounce the particle moves with the struck
// surface until its next contact: the remaining path of a later surface starts
// where that point was relative to it. Exact for constant surface velocities over
// the tick. Returns false when a fifth impact was needed (the state then stays at
// the fourth contact, separated). A candidate query must return every surface
// whose motion over the interval can meet the segment (bounds grown by the motion).
// A surface moved into emitter-origin space (points and its velocity centre).
// (a, b, c are offsets from the surface's origin: only the origin moves.)
NvSurface nv_surface_local(NvSurface s, nv_real3 origin) {
    s.origin = s.origin - origin;
    return s;
}
bool nv_collide(NvMotion m, nv_real h, nv_real3 start, nv_real3 move, NV_INOUT(NvState) s, NV_OUT(NvImpact) first) {
    first.count = 0u; first.contact = nv_make3(NV_R(0), NV_R(0), NV_R(0)); first.velocity = first.contact; first.normal = first.contact; first.fraction = NV_R(0);
    // Positions stay in origin space; the anchor-space point is formed only for
    // the surface tests, so a particle that hits nothing keeps its exact path.
    nv_real3 origin = m.origin_anchor;
    nv_real3 local = start, displacement = move, velocity = s.velocity;
    nv_real remaining = h;             // seconds of the interval not yet swept
    uint carrier = 0xffffffffu;        // surface the particle moves with since its last contact
    nv_real3 lastNormal = nv_make3(NV_R(0), NV_R(0), NV_R(0)); uint creases = 0u;
    bool complete = true;
    NV_LOOP for (uint bounce = 0u; bounce <= 4u; ++bounce) {
        nv_real earliest = NV_R(2); uint selected = 0xffffffffu; nv_real3 normal = nv_make3(NV_R(0), NV_R(0), NV_R(0));
        nv_real3 offset = nv_make3(NV_R(0), NV_R(0), NV_R(0)), path = displacement;
        // Where the particle was when this sweep's time started, relative to `local`
        // (it moved with its carrier since its last contact; zero before any contact).
        nv_real3 back = nv_make3(NV_R(0), NV_R(0), NV_R(0));
        if (carrier != 0xffffffffu) back = nv_surface_uncarry_delta(nv_surface_local(NV_SURFACE(carrier), origin), remaining, local);
        // Candidates may come from an acceleration structure (duplicates allowed):
        // the tie rule makes the result independent of their order. The query is
        // anchor space; the hit test is emitter-origin space (error ~ D 2^-24 with
        // D the distance from the emitter origin, not from the anchor).
        NV_SURFACE_QUERY_TYPE query = NV_SURFACE_QUERY(origin + local, displacement); uint n = 0u;
        NV_LOOP while (NV_SURFACE_NEXT(query, n)) {
            NvSurface surface = nv_surface_local(NV_SURFACE(n), origin);
            if (m.self == 0u && surface.entity0 == m.entity0 && surface.entity1 == m.entity1 && surface.generation0 == m.generation0 && surface.generation1 == m.generation1) continue;
            // The path relative to this surface starts at local + delta (tick-end frame).
            nv_real3 delta = nv_make3(NV_R(0), NV_R(0), NV_R(0));
            if (n != carrier) {
                delta = back;
                if (nv_surface_moves(surface)) delta = delta + nv_surface_carry_delta(surface, remaining, local + back);
            }
            nv_real3 d = displacement - delta;
            nv_real t; nv_real3 direction;
            if (nv_hit(surface, local, delta, d, t, direction) && (t < earliest || (t == earliest && n < selected))) { earliest = t; selected = n; normal = direction; offset = delta; path = d; }
        }
        if (selected == 0xffffffffu) { local = local + displacement; break; }
        if (bounce == 4u) { complete = false; break; }
        NvSurface hitSurface = nv_surface_local(NV_SURFACE(selected), origin);
        nv_real3 contactLocal = local + (offset + path * earliest);
        nv_real3 surfaceVelocity = hitSurface.velocity + cross(hitSurface.angular, contactLocal - hitSurface.origin);
        nv_real3 rest = path * (NV_R(1) - earliest), bounced = nv_bounce(rest, normal, m.restitution, m.friction);
        // s1: velocity at the contact of the last struck surface (none on the first contact).
        nv_real tau = remaining * (NV_R(1) - earliest); nv_real3 s1 = surfaceVelocity;
        if (first.count > 0u) { NvSurface previous = nv_surface_local(NV_SURFACE(carrier), origin); s1 = previous.velocity + cross(previous.angular, contactLocal - previous.origin); }
        // Measured in the last struck surface's frame: a closing gap brings it to the
        // particle even when the bounce itself leaves it.
        if (first.count > 0u && dot(bounced + (surfaceVelocity - s1) * tau, lastNormal) < NV_R(0)) {
            // Facing surfaces (a gap or crease: bouncing off this surface would drive
            // the particle back into the last struck one). For restitution < 1 the
            // limit of the bounces between them is no motion across either, each in
            // its own frame. With s1, s2 the two surface velocities at the contact:
            // v.n1 = s1.n1 and v.n2 = s2.n2, the crease component free (friction on
            // it). Nearly parallel surfaces share one normal, and the particle moves
            // with the midplane (a squeeze has no other answer). A third facing
            // contact (a corner, or a closing gap bringing a surface back) holds it
            // on this surface. The rest of the path is relative to this surface: it
            // follows the other surface's motion across n1 over the remaining time.
            nv_real3 s2 = surfaceVelocity; nv_real g = dot(lastNormal, normal);
            nv_real3 k = cross(lastNormal, normal); nv_real kk = dot(k, k);
            nv_real3 vrel = velocity - s2;
            if (creases == 0u && kk > NV_R(0.01)) {
                k = k * (NV_R(1) / sqrt(kk));
                nv_real r1 = dot(s1, lastNormal), r2 = dot(s2, normal);
                nv_real3 across = lastNormal * ((r1 - g * r2) / kk) + normal * ((r2 - g * r1) / kk);
                velocity = across + k * (dot(s2, k) + dot(vrel, k) * (NV_R(1) - m.friction));
                nv_real p1 = dot(s1 - s2, lastNormal) * tau;
                rest = lastNormal * (p1 / kk) + normal * (-g * p1 / kk) + k * (dot(rest, k) * (NV_R(1) - m.friction));
            } else if (creases == 0u) {
                nv_real mid = NV_R(0.5) * (dot(s2, normal) + dot(s1, normal));
                nv_real3 tangent = vrel - normal * dot(vrel, normal);
                velocity = s2 - normal * dot(s2, normal) + normal * mid + tangent * (NV_R(1) - m.friction);
                rest = (rest - normal * dot(rest, normal)) * (NV_R(1) - m.friction) + normal * ((mid - dot(s2, normal)) * tau);
            } else { velocity = s2; rest = nv_make3(NV_R(0), NV_R(0), NV_R(0)); }
            displacement = rest;
            creases = creases + 1u;
        } else {
            velocity = surfaceVelocity + nv_bounce(velocity - surfaceVelocity, normal, m.restitution, m.friction);
            // The rest of the path, relative to the struck surface, reflects off it.
            displacement = bounced;
        }
        first.count = first.count + 1u;
        if (first.count == 1u) { first.contact = contactLocal; first.velocity = velocity; first.normal = normal; first.fraction = earliest; }
        lastNormal = normal;
        local = contactLocal + normal * m.separation;
        remaining = remaining * (NV_R(1) - earliest);
        carrier = selected;
    }
    s.position = local; s.velocity = velocity;
    return complete;
}

// ---- one interval --------------------------------------------------------------------------
// Advances s by h seconds from its age. Forces are sampled at the start
// position; noise at the interval midpoint. Returns false on a fifth impact.
bool nv_integrate(NvMotion m, nv_real h, NvDrag d, NV_INOUT(NvState) s, NV_OUT(NvImpact) impact) {
    nv_real3 start = s.position, q = m.origin_anchor + s.position;
    nv_real3 a = m.acceleration;
    if (m.noise.x != NV_R(0) || m.noise.y != NV_R(0) || m.noise.z != NV_R(0)) {
        nv_real time = (s.age + h * NV_R(0.5)) * m.noise_frequency;
        a = a + nv_make3(m.noise.x * nv_smooth_noise(m.noise_seed, 0u, time), m.noise.y * nv_smooth_noise(m.noise_seed, 1u, time), m.noise.z * nv_smooth_noise(m.noise_seed, 2u, time));
    }
    a = a + nv_context_fields(q);
    a = a + nv_world_field(0u, q);
    // The interval's displacement is kept as a small vector and added to the
    // (possibly far from the origin) position once: the sweep uses it directly.
    nv_real3 move = s.velocity * d.position + a * d.acceleration;
    s.velocity = s.velocity * d.velocity + a * d.position;
    if (m.wind != 0u) {
        nv_real3 w = nv_world_field(1u, q);
        move = move + w * (m.drag * d.acceleration);
        s.velocity = s.velocity + w * (m.drag * d.position);
    }
    bool complete = true;
    impact.count = 0u; impact.contact = nv_make3(NV_R(0), NV_R(0), NV_R(0)); impact.velocity = impact.contact; impact.normal = impact.contact; impact.fraction = NV_R(0);
    if (m.collision != 0u) complete = nv_collide(m, h, start, move, s, impact);
    else s.position = start + move;
    s.age = s.age + h;
    return complete;
}

// ---- births --------------------------------------------------------------------------------
// Elapsed seconds from birth r of a record to the tick end.
nv_real nv_birth_elapsed(uint kind, uint rank, nv_real interval, nv_real carry, nv_real rate) {
    if (kind == 0u) return interval;
    return max(NV_R(0), interval - (NV_R(rank) + NV_R(1) - carry) / rate);
}
struct NvBirthShape {
    uint shape; nv_real position_radius; nv_real velocity_radius; nv_real3 box;
    nv_real3 cone; nv_real cone_cos; nv_real3 velocity; nv_real speed;
};
nv_real3 nv_unit_sphere_point(NV_INOUT(uint) rng, nv_real radius) {
    nv_real z = NV_R(2) * nv_next01(rng) - NV_R(1), theta = NV_R(6.283185307179586) * nv_next01(rng), r = radius * pow(nv_next01(rng), NV_R(1.0 / 3.0));
    nv_real xy = sqrt(max(NV_R(0), (NV_R(1) - z) * (NV_R(1) + z))); // 1 - z*z cancels near the poles; both factors are exact
    return nv_make3(r * xy * cos(theta), r * xy * sin(theta), r * z);
}
// Local offset and velocity of a generated birth (before source frames and inheritance).
void nv_birth_local(NvBirthShape b, uint rng, NV_OUT(nv_real3) offset, NV_OUT(nv_real3) velocity) {
    offset = nv_make3(NV_R(0), NV_R(0), NV_R(0));
    if (b.shape == 1u) offset = nv_unit_sphere_point(rng, b.position_radius);
    else if (b.shape == 2u) {
        // One draw per statement: argument evaluation order differs between C++ and HLSL.
        nv_real bx = (NV_R(2) * nv_next01(rng) - NV_R(1)) * b.box.x;
        nv_real by = (NV_R(2) * nv_next01(rng) - NV_R(1)) * b.box.y;
        nv_real bz = (NV_R(2) * nv_next01(rng) - NV_R(1)) * b.box.z;
        offset = nv_make3(bx, by, bz);
    }
    else if (b.shape == 3u) { nv_real theta = NV_R(6.283185307179586) * nv_next01(rng), r = b.position_radius * sqrt(nv_next01(rng)); offset = nv_make3(r * cos(theta), NV_R(0), r * sin(theta)); }
    velocity = nv_unit_sphere_point(rng, b.velocity_radius);
    nv_real speed = length(b.cone);
    if (speed > NV_R(0)) {
        nv_real3 n = b.cone * (NV_R(1) / speed);
        nv_real3 u = abs(n.z) < NV_R(0.9) ? nv_make3(-n.y, n.x, NV_R(0)) : nv_make3(NV_R(0), -n.z, n.y);
        u = u * (NV_R(1) / length(u));
        nv_real3 v = cross(n, u);
        // w = 1 - z is formed directly: 1 - z*z would cancel for narrow cones
        // (radial error eps / radial), while w * (2 - w) keeps relative precision.
        nv_real w = nv_next01(rng) * (NV_R(1) - b.cone_cos), z = NV_R(1) - w;
        nv_real radial = sqrt(max(NV_R(0), w * (NV_R(2) - w))), theta = NV_R(6.283185307179586) * nv_next01(rng);
        velocity = velocity + (n * z + (u * cos(theta) + v * sin(theta)) * radial) * speed;
    }
    velocity = (velocity + b.velocity) * b.speed;
}
// Row-major 3x4 affine applied to a point / vector.
nv_real3 nv_affine_vector(nv_real4 r0, nv_real4 r1, nv_real4 r2, nv_real3 v) { return nv_make3(r0.x * v.x + r0.y * v.y + r0.z * v.z, r1.x * v.x + r1.y * v.y + r1.z * v.z, r2.x * v.x + r2.y * v.y + r2.z * v.z); }
nv_real3 nv_affine_point(nv_real4 r0, nv_real4 r1, nv_real4 r2, nv_real3 p) { return nv_affine_vector(r0, r1, r2, p) + nv_make3(r0.w, r1.w, r2.w); }

// Full generated birth in origin space at the birth instant (the state its birth
// event reports). fraction = (dt - elapsed) / dt of the tick. Source maps are
// row-major [R | t - origin]; they are used only when source != 0.
// Order: local = spawn_offset + shape offset; velocity = (sphere + cone +
// constant) * speed; source: p = (1-f) P(local) + f C(local), v = (1-f) Rp v +
// f Rc v; finally v += inherited.
NvState nv_birth_state(NvBirthShape b, uint rng, uint source,
                       nv_real4 p0, nv_real4 p1, nv_real4 p2, nv_real4 c0, nv_real4 c1, nv_real4 c2,
                       nv_real3 spawn_offset, nv_real3 inherited, nv_real fraction) {
    nv_real3 offset, velocity;
    nv_birth_local(b, rng, offset, velocity);
    nv_real3 local = spawn_offset + offset;
    NvState s;
    if (source != 0u) {
        nv_real f = fraction;
        s.position = nv_affine_point(p0, p1, p2, local) * (NV_R(1) - f) + nv_affine_point(c0, c1, c2, local) * f;
        s.velocity = nv_affine_vector(p0, p1, p2, velocity) * (NV_R(1) - f) + nv_affine_vector(c0, c1, c2, velocity) * f;
    } else { s.position = local; s.velocity = velocity; }
    s.velocity = s.velocity + inherited;
    s.age = NV_R(0);
    return s;
}

#endif
