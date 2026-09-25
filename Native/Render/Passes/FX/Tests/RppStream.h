#pragma once
// Test and gate fixture: a stand-in for the VFX CPU authority that writes the particle stream (NativeVfxStream.h) of
// the RPP load (WORLD_VFX_DESIGN_KO.md 3.3): 128 root emitters x 2048 births/s with lifetime 2 s -> 524,288 live
// particles in steady state (from tick 121 on), dt 1/60, 16 context fields (acceleration, radial, vortex), world
// gravity and wind, noise, linear drag, ground collision (two triangles) with collision events on 1/8 of the programs,
// death events on 1/4. On top of it the features the module must carry exactly (RppConfig::features):
//   - same-tick child cascades (user decision 8.1 (b)): roots 0 and 64 have a death rule -> C1 (burst 2, 4 ms life,
//     death rule) -> C2 (burst 2, 4 ms, death and birth rules) -> C3 (burst 1, 4 ms, death rule) -> C4 (burst 1, 0.5 s);
//     C2's birth rule -> CB (burst 1, 0.3 s). A child expiring inside its tick cascades in the same tick (up to depth
//     4); the others die in a later tick (depth 0) and cascade from there;
//   - a moving source (rows e % 16 == 5), attached transport (e % 16 == 7), rebase (e % 16 == 9, every 50 ticks),
//     explicit births (row 3: its births come as CPU-sampled records), a kill (row 10 at tick 400).
// Every discrete decision (births, deaths, event slots, children, counts, capacity) is made here in double, as the
// stream contract requires; the GPU module and the CPU reference executor only fill continuous state.
// Collision surfaces: the ground (two anchor-space triangles) and RppConfig::bodies rigid bodies, each with a sphere, a
// capsule and a two-triangle plate in body space; the body frames (translation, rotation about y) come every tick.
#include "NativeVfxStream.h"
#include "../Stream/src/VfxStreamCpu.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <vector>

namespace unx::fx::test
{
struct RppConfig
{
    uint32_t emitters = 128;
    uint32_t particles = 524288;  // live particles of the root emitters in steady state
    double lifetime = 2.0;
    double dt = 1.0 / 60.0;
    uint32_t fields = 16;
    bool features = true;         // cascades, source, transport, rebase, explicit births, kill
    uint32_t killRow = 10, killTick = 400;
    bool boxShape = true;         // program 2 emits from a box (development switch while the shared box draws are fixed)
    uint32_t bodies = 1728;
    double anchorShift[3] = { 0, 0, 0 };  // diagnostic: the anchor moved by this much (world stays the same)
    bool delta = true;            // emitter table as NV_STREAM_EMITTER_DELTA packets (rows that changed since they were last sent)
    bool childNoise = true;       // diagnostic switch: noise on the cascade children's programs       // rigid bodies with 4 collision surfaces each (RPP: 6,912 surfaces), moving and rotating
};

class RppStream
{
public:
    explicit RppStream(const RppConfig& c) : m_c(c)
    {
        m_anchor[0] = 1000.0 + c.anchorShift[0]; m_anchor[1] = 0.0 + c.anchorShift[1]; m_anchor[2] = -2000.0 + c.anchorShift[2];
        buildPrograms();
        const double rate = double(c.particles) / c.emitters / c.lifetime;
        for (uint32_t e = 0; e < c.emitters; ++e)
        {
            Row r;
            r.active = true;
            r.program = e % 16;
            r.rate = rate;
            r.duration = 1e30;
            r.origin[0] = m_anchor[0] - c.anchorShift[0] + 6.0 * (e % 16);  // world fixed under an anchor shift
            r.origin[1] = m_anchor[1] - c.anchorShift[1] + 2.0;
            r.origin[2] = m_anchor[2] - c.anchorShift[2] + 6.0 * (e / 16);
            r.seed = 0x9E3779B9u * (e + 1);
            r.explicitBirths = c.features && e == 3;
            r.source = c.features && e % 16 == 5;
            r.transport = c.features && e % 16 == 7;
            r.rebase = c.features && e % 16 == 9;
            m_rows.push_back(r);
        }
        if (c.features && c.emitters > 64)
        {
            m_rows[0].program = 16;  // death rule -> C1 cascade
            m_rows[64].program = 16;
        }
    }

    uint64_t tick() const { return m_tick; }
    uint32_t capacity() const { return m_capacity; }
    uint32_t aliveAfter() const { return m_aliveAfter; }
    uint32_t rows() const { return (uint32_t)m_rows.size(); }
    uint32_t eventSlots() const { return m_eventSlots; }
    uint32_t childRowsCreated() const { return m_childRows; }
    uint32_t maxDepth() const { return m_maxDepth; }
    uint32_t capacityChanges() const { return m_capacityChanges; }
    uint32_t sentBlocks() const { return m_sentBlocks; }  // emitter blocks of the last packet (delta)

    // Packet of the next tick. 'previous' = the readback events of the previous tick (the CPU authority reads the
    // GPU's event states: child origins and inherited velocities).
    std::vector<uint8_t> next(const std::vector<NV_StreamEvent>* previous)
    {
        ++m_tick;
        const bool first = m_tick == 1;
        const double dt = m_c.dt, tEnd = m_tick * dt, tStart = tEnd - dt;

        // Children created last tick become ordinary rows: origin = float(parent's origin_anchor of that tick + event
        // position), inherited = ratio x event velocity (parents before children: creation order).
        for (uint32_t row : m_created)
        {
            Row& r = m_rows[row];
            if (!previous || r.parentEvent >= previous->size()) throw std::runtime_error("RppStream: missing readback for a child row");
            const NV_StreamEvent& ev = (*previous)[r.parentEvent];
            const Row& parent = m_rows[r.parent];
            for (int a = 0; a < 3; ++a)
            {
                r.originAnchor[a] = parent.originAnchorTick[a] + ev.position[a];
                r.origin[a] = m_anchor[a] + (double)r.originAnchor[a];
                r.inheritedNow[a] = ev.velocity[a] * r.inherit;
            }
            r.originAnchorTick[0] = r.originAnchor[0]; r.originAnchorTick[1] = r.originAnchor[1]; r.originAnchorTick[2] = r.originAnchor[2];
            r.parentEvent = NV_STREAM_NONE;
        }
        m_created.clear();

        // Rows of this tick: rebase, kill.
        for (uint32_t e = 0; e < (uint32_t)m_rows.size(); ++e)
        {
            Row& r = m_rows[e];
            r.rebaseNow = 0;
            r.killed = r.active && m_c.features && e == m_c.killRow && m_tick == m_c.killTick;
            if (r.active && r.rebase && m_tick % 50 == 0) { r.rebaseNow = 0.5; r.origin[0] += 0.5; }
            if (!r.childRow) for (int a = 0; a < 3; ++a) r.originAnchor[a] = (float)(r.origin[a] - m_anchor[a]);
            for (int a = 0; a < 3; ++a) r.originAnchorTick[a] = r.originAnchor[a];
        }

        std::vector<NV_StreamSpawn> spawns[NV_STREAM_MAX_DEPTH + 1];
        std::vector<NV_StreamExplicitBirth> explicitBirths;
        m_events.clear();
        uint32_t aliveBefore = 0;

        // Deaths of existing particles (depth 0): births whose age at tEnd reaches the lifetime (ages decrease with the
        // birth number, so the dead births are a prefix).
        for (uint32_t e = 0; e < (uint32_t)m_rows.size(); ++e)
        {
            Row& r = m_rows[e];
            r.dyingBirth = r.deathBirth;
            r.deathEvent = NV_STREAM_NONE;
            if (!r.active) continue;
            aliveBefore += r.nextBirth - r.deathBirth;
            if (r.killed) { r.deathBirth = r.nextBirth; r.batches.clear(); continue; }
            const Program& p = m_programs[r.program];
            uint32_t death = r.nextBirth;
            for (const Batch& b : r.batches)
            {
                uint32_t lo = 0, hi = b.count;
                while (lo < hi)
                {
                    const uint32_t mid = (lo + hi) / 2;
                    if (tEnd - b.birthTime(mid) < p.lifetime) hi = mid; else lo = mid + 1;
                }
                if (lo < b.count) { death = b.first + lo; break; }
            }
            if (p.deathRule && death != r.dyingBirth)
            {
                r.deathEvent = (uint32_t)m_events.size();
                for (uint32_t b = r.dyingBirth; b != death; ++b) m_events.push_back({ e, b, r.birthTime(b) + p.lifetime, 0u, true });
            }
            r.deathBirth = death;
            while (!r.batches.empty() && r.batches.front().first + r.batches.front().count <= death) r.batches.pop_front();
        }

        // Depth 0 births of the root rows (scheduled; row 3 as explicit records).
        for (uint32_t e = 0; e < (uint32_t)m_rows.size(); ++e)
        {
            Row& r = m_rows[e];
            if (!r.active || r.childRow || r.killed) continue;
            const Program& p = m_programs[r.program];
            const double active = std::max(0.0, std::min(dt, r.duration - r.age));
            const double total = r.carry + r.rate * active;
            const uint32_t count = (uint32_t)std::floor(total);
            if (count)
            {
                Batch b{ r.nextBirth, count, tStart, r.carry, r.rate, false, 0 };
                if (r.explicitBirths)
                {
                    for (uint32_t k = 0; k < count; ++k)
                    {
                        NV_StreamExplicitBirth x{};
                        x.emitter = e;
                        x.birth = r.nextBirth + k;
                        x.birth_event = NV_STREAM_NONE;
                        x.death_event = NV_STREAM_NONE;
                        const double u = x.birth * 0.6180339887, th = 6.283185307179586 * (u - std::floor(u));
                        x.position[0] = (float)(0.4 * std::cos(th)); x.position[2] = (float)(0.4 * std::sin(th));
                        x.velocity[1] = 1.5f;
                        x.elapsed = (float)(tEnd - b.birthTime(k));
                        explicitBirths.push_back(x);
                    }
                }
                else
                {
                    uint32_t expired = 0;
                    while (expired < count && tEnd - b.birthTime(expired) >= p.lifetime) ++expired;
                    addRecord(spawns[0], e, NV_STREAM_SPAWN_SCHEDULED, r.nextBirth, count, expired, (float)dt, (float)r.carry, (float)r.rate, tEnd, 0);
                    if (r.deathBirth == r.nextBirth) r.deathBirth += expired;
                }
                r.batches.push_back(b);
                r.nextBirth += count;
            }
            r.carry = total - count;
            r.age += dt;
        }

        // Children: events of depth d-1 -> child rows with burst records of depth d.
        size_t scan = 0;
        for (uint32_t d = 1; d <= NV_STREAM_MAX_DEPTH && m_c.features; ++d)
        {
            const size_t end = m_events.size();
            for (size_t i = scan; i < end; ++i)
            {
                const EventTime ev = m_events[i];
                if (ev.depth != d - 1) continue;
                const Program& pp = m_programs[m_rows[ev.row].program];
                const uint32_t childProgram = ev.death ? pp.deathChild : pp.birthChild;
                if (childProgram == NV_STREAM_NONE) continue;
                const Program& cp = m_programs[childProgram];
                const uint32_t burst = ev.death ? pp.deathBurst : pp.birthBurst;
                const uint32_t row = allocateRow();
                Row& c = m_rows[row];
                c = Row{};
                c.active = true;
                c.childRow = true;
                c.program = childProgram;
                c.parent = ev.row;
                c.parentEvent = (uint32_t)i;
                c.inherit = 0.5f;
                c.seed = 0xA511E9B3u ^ (uint32_t)(i * 2654435761u) ^ (uint32_t)(m_tick * 40503u);
                const double interval = tEnd - ev.time;  // event -> tick end
                const Batch b{ 0, burst, 0, 0, 0, true, ev.time };
                const uint32_t expired = interval >= cp.lifetime ? burst : 0;
                addRecord(spawns[d], row, NV_STREAM_SPAWN_BURST, 0, burst, expired, (float)interval, 0, 0, tEnd, d);
                c.batches.push_back(b);
                c.nextBirth = burst;
                c.deathBirth = expired;
                c.dyingBirth = 0;
                m_created.push_back(row);
                ++m_childRows;
                m_maxDepth = std::max(m_maxDepth, d);
            }
            scan = end;
        }

        // Totals.
        uint32_t alive = 0, collisionCapacity = 0, slotBirths = (uint32_t)explicitBirths.size(), ribbonPoints = 0, mediumCells = 0;
        for (uint32_t d = 0; d <= NV_STREAM_MAX_DEPTH; ++d)
            for (const auto& s : spawns[d]) slotBirths += s.count - s.expired;
        for (auto& r : m_rows)
        {
            if (!r.active) continue;
            alive += r.nextBirth - r.deathBirth;
            // output bases: ribbon points and medium cells of the live births [death_birth, next_birth)
            const NV_StreamProgram& pr = m_programs[r.program].record;
            r.outputBase = 0;
            if (pr.output == 2) { r.outputBase = ribbonPoints; ribbonPoints += r.nextBirth - r.deathBirth; }
            if (pr.output == 3) { const uint32_t g = pr.medium_grid; r.outputBase = mediumCells; mediumCells += (r.nextBirth - r.deathBirth) * g * g * g; }
            if (m_programs[r.program].collisionEvents) collisionCapacity += r.nextBirth - r.dyingBirth;
        }
        m_aliveAfter = alive;
        // Slot capacity as the VFX authority sizes it: need = live at the tick start + slot-taking births; grow when the
        // need exceeds the capacity, shrink when it falls below a quarter; new capacity = need x 1.25 rounded up to 64.
        const uint32_t need = aliveBefore + slotBirths;
        if (need > m_capacity || need < m_capacity / 4)
        {
            const uint32_t target = std::max<uint32_t>((uint32_t)((uint64_t)need * 5 / 4), 64);
            m_capacity = (target + 63) / 64 * 64;
            ++m_capacityChanges;
        }
        m_eventSlots = (uint32_t)m_events.size();

        // Packet. Emitter table: whole on the first packet (RESET), else the rows whose block differs from the one the GPU
        // holds (the last sent block with its per-tick fields read as absent, NativeVfxStream.h NV_STREAM_EMITTER_DELTA).
        std::vector<NV_StreamEmitter> blocks;
        std::vector<uint32_t> blockRows;
        const bool delta = m_c.delta && !first;
        if (delta)
        {
            // a row inactive now and at its last send keeps its block (flags 0, no per-tick field): only the others are
            // built and compared
            const uint32_t sentRows = (uint32_t)m_sent.size();
            m_sent.resize(m_rows.size());
            for (uint32_t e = 0; e < (uint32_t)m_rows.size(); ++e)
            {
                if (e < sentRows && !m_rows[e].active && !(m_sent[e].flags & NV_STREAM_EMITTER_ACTIVE)) continue;
                const NV_StreamEmitter record = emitterRecord(e, dt);
                NV_StreamEmitter held{};
                if (e < sentRows)
                {
                    held = m_sent[e];
                    held.rebase[0] = held.rebase[1] = held.rebase[2] = 0;
                    held.flags &= ~uint32_t(NV_STREAM_EMITTER_TRANSPORT | NV_STREAM_EMITTER_SOURCE | NV_STREAM_EMITTER_KILLED);
                    held.parent_event = held.parent_row = NV_STREAM_NONE;
                }
                if (e >= sentRows || std::memcmp(&held, &record, sizeof held) != 0)
                {
                    blocks.push_back(record);
                    blockRows.push_back(e);
                }
                m_sent[e] = record;
            }
        }
        else
        {
            blocks.resize(m_rows.size());
            for (uint32_t e = 0; e < (uint32_t)m_rows.size(); ++e) { blocks[e] = emitterRecord(e, dt); blockRows.push_back(e); }
            m_sent = blocks;
        }
        m_sentBlocks = (uint32_t)blocks.size();
        std::vector<NV_StreamSpawn> all;
        NV_StreamHeader h{};
        for (uint32_t d = 0; d <= NV_STREAM_MAX_DEPTH; ++d)
        {
            h.depth[d] = (uint32_t)all.size();
            all.insert(all.end(), spawns[d].begin(), spawns[d].end());
        }
        h.depth[NV_STREAM_MAX_DEPTH + 1] = (uint32_t)all.size();
        h.magic = NV_STREAM_MAGIC;
        h.version = NV_STREAM_VERSION;
        h.flags = first ? (NV_STREAM_RESET | NV_STREAM_PROGRAMS | NV_STREAM_SURFACES) : (delta ? NV_STREAM_EMITTER_DELTA : 0u);
        h.stream = 0x5354524541ull;
        h.generation = 1;
        h.tick = m_tick;
        h.dt = dt;
        h.time = tEnd;
        std::memcpy(h.anchor, m_anchor, sizeof m_anchor);
        h.dt_float = (float)dt;
        h.time_float = (float)tEnd;
        h.slot_capacity = m_capacity;
        h.alive_after = alive;
        h.program_count = first ? (uint32_t)m_programs.size() : 0;
        h.curve_key_count = first ? (uint32_t)m_keys.size() : 0;
        h.emitter_count = (uint32_t)blocks.size();
        h.emitter_table = (uint32_t)m_rows.size();
        h.spawn_count = (uint32_t)all.size();
        h.explicit_count = (uint32_t)explicitBirths.size();
        h.field_count = (uint32_t)m_fields.size();
        h.world_field_count = (uint32_t)m_world.size();
        h.surface_count = first ? (uint32_t)m_surfaces.size() : 0;
        const std::vector<NV_StreamBody> bodies = bodyFrames(tEnd);
        h.body_count = (uint32_t)bodies.size();
        const std::vector<NV_StreamSurface> dynamicRows = dynamicSurfaces(tEnd);
        h.dynamic_surface_count = (uint32_t)dynamicRows.size();
        h.event_slots = m_eventSlots;
        h.collision_capacity = collisionCapacity;
        h.ribbon_points = ribbonPoints;
        h.medium_cells = mediumCells;

        std::vector<uint8_t> packet(sizeof(NV_StreamHeader));
        auto section = [&](const void* data, size_t bytes) -> uint64_t {
            if (!bytes) return 0;
            packet.resize((packet.size() + 15) & ~size_t(15));
            const uint64_t at = packet.size();
            packet.insert(packet.end(), (const uint8_t*)data, (const uint8_t*)data + bytes);
            return at;
        };
        std::vector<NV_StreamProgram> programs;
        for (const auto& p : m_programs) programs.push_back(p.record);
        if (first)
        {
            h.programs = section(programs.data(), programs.size() * sizeof(NV_StreamProgram));
            h.curve_keys = section(m_keys.data(), m_keys.size() * sizeof(NV_StreamCurveKey));
        }
        h.emitters = section(blocks.data(), blocks.size() * sizeof(NV_StreamEmitter));
        if (delta) h.emitter_rows = section(blockRows.data(), blockRows.size() * 4);
        h.spawns = section(all.data(), all.size() * sizeof(NV_StreamSpawn));
        h.explicit_births = section(explicitBirths.data(), explicitBirths.size() * sizeof(NV_StreamExplicitBirth));
        h.fields = section(m_fields.data(), m_fields.size() * sizeof(NV_StreamField));
        h.world_fields = section(m_world.data(), m_world.size() * sizeof(NV_StreamWorldField));
        if (first) h.surfaces = section(m_surfaces.data(), m_surfaces.size() * sizeof(NV_StreamSurface));
        h.bodies = section(bodies.data(), bodies.size() * sizeof(NV_StreamBody));
        h.dynamic_surfaces = section(dynamicRows.data(), dynamicRows.size() * sizeof(NV_StreamSurface));
        packet.resize((packet.size() + 15) & ~size_t(15));
        h.bytes = packet.size();
        std::memcpy(packet.data(), &h, sizeof h);

        // Rows: a killed row ends; a child row whose last particle died is free from the next tick.
        for (uint32_t e = 0; e < (uint32_t)m_rows.size(); ++e)
        {
            Row& r = m_rows[e];
            if (r.killed) r.active = false;
            if (r.active && r.childRow && r.nextBirth == r.deathBirth && r.parentEvent == NV_STREAM_NONE)
            {
                r.active = false;
                freeRow(e);
            }
        }
        return packet;
    }

private:
    struct Batch
    {
        uint32_t first, count;
        double tStart, carry, rate;
        bool burst;
        double burstTime;
        double birthTime(uint32_t k) const { return burst ? burstTime : tStart + (k + 1 - carry) / rate; }
    };
    struct Program
    {
        NV_StreamProgram record{};
        double lifetime = 2;
        bool deathRule = false, birthRule = false, collisionEvents = false;
        uint32_t deathChild = NV_STREAM_NONE, birthChild = NV_STREAM_NONE, deathBurst = 0, birthBurst = 0;
    };
    struct Row
    {
        bool active = false, childRow = false, killed = false, explicitBirths = false, source = false, transport = false, rebase = false;
        uint32_t program = 0, parent = NV_STREAM_NONE, parentEvent = NV_STREAM_NONE;
        uint32_t nextBirth = 0, deathBirth = 0, dyingBirth = 0, deathEvent = NV_STREAM_NONE, outputBase = 0;
        double rate = 0, carry = 0, age = 0, duration = 0, rebaseNow = 0;
        double origin[3] = {};
        float originAnchor[3] = {}, originAnchorTick[3] = {}, inheritedNow[3] = {};
        float inherit = 0;
        uint32_t seed = 0;
        std::deque<Batch> batches;
        double birthTime(uint32_t b) const
        {
            for (const auto& x : batches)
                if (b - x.first < x.count) return x.birthTime(b - x.first);
            throw std::runtime_error("RppStream: birth outside the live batches");
        }
    };
    struct EventTime { uint32_t row, birth; double time; uint32_t depth; bool death; };

    void addRecord(std::vector<NV_StreamSpawn>& list, uint32_t row, uint32_t kind, uint32_t firstBirth, uint32_t count, uint32_t expired, float interval,
                   float carry, float rate, double tEnd, uint32_t depth)
    {
        const Program& p = m_programs[m_rows[row].program];
        NV_StreamSpawn s{};
        s.emitter = row; s.count = count; s.first_birth = firstBirth; s.expired = expired; s.kind = kind;
        if (!list.empty()) s.thread_offset = list.back().thread_offset + list.back().count;  // prefix of the depth's threads
        s.interval = interval; s.carry = carry; s.rate = rate;
        s.birth_event = NV_STREAM_NONE;
        s.death_event = NV_STREAM_NONE;
        const Batch b{ firstBirth, count, tEnd - m_c.dt, carry, rate, kind == NV_STREAM_SPAWN_BURST, tEnd - interval };
        if (p.birthRule)
        {
            s.birth_event = (uint32_t)m_events.size();
            for (uint32_t k = 0; k < count; ++k) m_events.push_back({ row, firstBirth + k, b.birthTime(k), depth, false });
        }
        if (p.deathRule && expired)
        {
            s.death_event = (uint32_t)m_events.size();
            for (uint32_t k = 0; k < expired; ++k) m_events.push_back({ row, firstBirth + k, b.birthTime(k) + p.lifetime, depth, true });
        }
        list.push_back(s);
    }

    // The lowest inactive child row (a min-heap of the rows freed so far), else a new row.
    uint32_t allocateRow()
    {
        if (!m_freeRows.empty())
        {
            std::pop_heap(m_freeRows.begin(), m_freeRows.end(), std::greater<uint32_t>());
            const uint32_t e = m_freeRows.back();
            m_freeRows.pop_back();
            return e;
        }
        m_rows.emplace_back();
        return (uint32_t)m_rows.size() - 1;
    }
    void freeRow(uint32_t e)
    {
        m_freeRows.push_back(e);
        std::push_heap(m_freeRows.begin(), m_freeRows.end(), std::greater<uint32_t>());
    }

    NV_StreamEmitter emitterRecord(uint32_t e, double dt)
    {
        const Row& r = m_rows[e];
        const Program& p = m_programs[r.program];
        NV_StreamEmitter x{};
        std::memcpy(x.origin, r.origin, sizeof x.origin);
        x.program = r.program;
        x.flags = r.active ? NV_STREAM_EMITTER_ACTIVE : 0u;
        if (r.active && r.killed) x.flags |= NV_STREAM_EMITTER_KILLED;
        if (r.active && p.deathRule) x.flags |= NV_STREAM_EMITTER_DEATH_EVENTS;
        for (int a = 0; a < 3; ++a) x.origin_anchor[a] = r.originAnchor[a];
        x.rebase[0] = (float)r.rebaseNow;
        x.rng_key = r.seed ^ 0x68E31DA4u;
        x.noise_key = r.seed * 0x85EBCA6Bu;
        x.next_birth = r.nextBirth;
        x.death_birth = r.deathBirth;
        x.dying_birth = r.dyingBirth;
        x.death_event = r.deathEvent;
        x.speed = 1;
        x.size_scale = 1;
        nv_stream::Math math;
        const auto drag = math.nv_linear_drag(p.record.drag, dt);
        x.drag_velocity = (float)drag.velocity;
        x.drag_position = (float)drag.position;
        x.drag_acceleration = (float)drag.acceleration;
        for (int c = 0; c < 4; ++c) x.color_scale[c] = 1;
        if (r.active && r.parentEvent != NV_STREAM_NONE)
        {
            x.parent_event = r.parentEvent;  // resolved by the GPU in this tick
            x.parent_row = r.parent;
            x.inherited[0] = r.inherit;
        }
        else
        {
            x.parent_event = NV_STREAM_NONE;
            x.parent_row = NV_STREAM_NONE;
            for (int a = 0; a < 3; ++a) x.inherited[a] = r.inheritedNow[a];
        }
        const float identity[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
        std::memcpy(x.transport, identity, sizeof identity);
        std::memcpy(x.source_previous, identity, sizeof identity);
        std::memcpy(x.source_current, identity, sizeof identity);
        if (r.source && r.active)
        {
            x.flags |= NV_STREAM_EMITTER_SOURCE;
            const double t1 = m_tick * dt, t0 = t1 - dt;
            x.source_previous[3] = (float)(3.0 * std::sin(0.5 * t0));
            x.source_current[3] = (float)(3.0 * std::sin(0.5 * t1));
            x.source_previous[11] = (float)std::cos(0.7 * t0);
            x.source_current[11] = (float)std::cos(0.7 * t1);
        }
        if (r.transport && r.active)
        {
            x.flags |= NV_STREAM_EMITTER_TRANSPORT;
            const double a = 0.3 * dt, c = std::cos(a), s = std::sin(a);
            const float rot[12] = { (float)c, 0, (float)s, 0, 0, 1, 0, 0, (float)-s, 0, (float)c, 0 };
            std::memcpy(x.transport, rot, sizeof rot);
        }
        x.entity[0] = e + 1;
        x.generation[0] = 1;
        x.output_base = r.outputBase;
        return x;
    }

    void buildPrograms()
    {
        m_keys.push_back({ 0, { 1, 1, 1 } });
        m_keys.push_back({ 1, { 1, 1, 1 } });
        for (uint32_t p = 0; p < 16; ++p)
        {
            Program x;
            NV_StreamProgram& r = x.record;
            r.output = 0;
            r.shape = NV_STREAM_SHAPE_SPHERE;
            r.lifetime = (float)m_c.lifetime;
            r.drag = 0.1f;
            r.position_radius = 0.5f;
            r.velocity_radius = 0.3f;
            r.velocity[0] = 0.2f * (p % 4) - 0.3f; r.velocity[1] = 3 + 0.1f * p; r.velocity[2] = 0.2f * (p / 4) - 0.3f;
            r.size = 0.05f;
            r.noise[0] = r.noise[1] = r.noise[2] = 0.5f;
            r.noise_frequency = 2;
            r.flags = NV_STREAM_PROGRAM_NOISE | NV_STREAM_PROGRAM_WIND;
            if (p % 8 == 0)
            {
                r.flags |= NV_STREAM_PROGRAM_COLLISION | NV_STREAM_PROGRAM_COLLISION_EVENTS;
                r.restitution = 0.5f; r.friction = 0.2f; r.separation = 0.001f;
                x.collisionEvents = true;
            }
            if (p % 4 == 0) { x.deathRule = true; r.flags |= NV_STREAM_PROGRAM_DEATH_EVENTS; }  // e.g. surface deposits
            if (p == 2 && m_c.boxShape) { r.shape = NV_STREAM_SHAPE_BOX; r.box[0] = 0.3f; r.box[1] = 0.1f; r.box[2] = 0.2f; }
            if (p == 6) { r.shape = NV_STREAM_SHAPE_DISC; r.position_radius = 0.7f; r.cone[1] = 2; r.cone_cos = 0.9f; }
            if (p == 11) r.shape = NV_STREAM_SHAPE_POINT;
            r.color[0] = r.color[1] = r.color[2] = r.color[3] = 1;
            r.size_count = r.color_count = r.alpha_count = 2;
            r.uv[0] = r.uv[1] = 1;
            r.columns = r.rows = 1;
            r.medium_grid = 1;
            r.ribbon_break = 3.4e38f;
            if (p == 13)  // ribbon trails: strips break at 1 m
            {
                r.output = 2;
                r.ribbon_normal[1] = 1; r.ribbon_uv = 1; r.ribbon_break = 1.0f;
            }
            if (p == 14)  // volume puffs: 2^3 medium cells per particle
            {
                r.output = 3;
                r.medium_grid = 2;
                r.medium_absorption[0] = r.medium_absorption[1] = r.medium_absorption[2] = 0.02f;
                r.medium_scattering[0] = r.medium_scattering[1] = r.medium_scattering[2] = 0.3f;
                r.medium_emission[0] = 0.1f;
                r.medium_phase = 0.4f;
            }
            x.lifetime = r.lifetime;
            m_programs.push_back(x);
        }
        Program root = m_programs[0];  // 16: collides, death rule -> C1
        root.deathChild = 17;
        root.deathBurst = 2;
        m_programs.push_back(root);
        auto child = [&](double lifetime, uint32_t deathChild, uint32_t deathBurst, uint32_t birthChild, uint32_t birthBurst) {
            Program x = m_programs[1];
            x.record.lifetime = (float)lifetime;
            x.record.flags = NV_STREAM_PROGRAM_NOISE | NV_STREAM_PROGRAM_WIND;
            x.record.velocity_radius = 1.0f;
            if (!m_c.childNoise) { x.record.noise[0] = x.record.noise[1] = x.record.noise[2] = 0; }
            x.lifetime = x.record.lifetime;
            x.collisionEvents = false;
            x.deathRule = deathChild != NV_STREAM_NONE;
            x.birthRule = birthChild != NV_STREAM_NONE;
            if (x.deathRule) x.record.flags |= NV_STREAM_PROGRAM_DEATH_EVENTS;
            if (x.birthRule) x.record.flags |= NV_STREAM_PROGRAM_BIRTH_EVENTS;
            x.deathChild = deathChild; x.deathBurst = deathBurst; x.birthChild = birthChild; x.birthBurst = birthBurst;
            m_programs.push_back(x);
        };
        child(0.004, 18, 2, NV_STREAM_NONE, 0);            // 17 C1
        child(0.004, 19, 2, 21, 1);                        // 18 C2
        child(0.004, 20, 1, NV_STREAM_NONE, 0);            // 19 C3
        child(0.5, NV_STREAM_NONE, 0, NV_STREAM_NONE, 0);  // 20 C4
        child(0.3, NV_STREAM_NONE, 0, NV_STREAM_NONE, 0);  // 21 CB

        // 16 context fields (the reference measurement's), anchor space
        for (uint32_t f = 0; f < m_c.fields; ++f)
        {
            NV_StreamField x{};
            x.kind = f % 3;
            x.position[0] = (float)(10.0 * f - m_c.anchorShift[0]); x.position[1] = (float)(5.0 - m_c.anchorShift[1]);
            x.position[2] = (float)(10.0 * (f % 4) - m_c.anchorShift[2]);
            x.value[0] = f % 3 == 1 ? 2.f : .1f;
            x.value[1] = f % 3 == 0 ? -.1f : 0;
            x.radius = 1;
            m_fields.push_back(x);
        }
        // world gravity and wind (global, add)
        NV_StreamWorldField g{};
        g.packed = 0u;
        g.inverse_basis[0] = g.inverse_basis[4] = g.inverse_basis[8] = 1;
        g.value[1] = -9.81f;
        m_world.push_back(g);
        NV_StreamWorldField w = g;
        w.packed = 1u;
        w.value[0] = 1; w.value[1] = 0;
        m_world.push_back(w);
        // ground: two triangles at world y = 0 under the emitters (anchor space)
        const float y = (float)(0.0 - m_anchor[1]), x0 = -100, x1 = 300, z0 = -100, z1 = 300;
        NV_StreamSurface t{};
        t.kind = 2;
        t.body = NV_STREAM_NONE;
        t.entity[0] = 99999;
        t.generation0 = 1;
        // world-space surfaces: origin = centroid (anchor space), a, b, c = offsets from it (NativeVfxStream.h)
        auto tri = [&](float ax, float az, float bx, float bz, float cx, float cz) {
            NV_StreamSurface s = t;
            const double ox = ((double)ax + bx + cx) / 3, oz = ((double)az + bz + cz) / 3;
            s.origin[0] = (float)ox; s.origin[1] = y; s.origin[2] = (float)oz;
            s.a[0] = (float)(ax - (double)s.origin[0]); s.a[1] = 0; s.a[2] = (float)(az - (double)s.origin[2]);
            s.b[0] = (float)(bx - (double)s.origin[0]); s.b[1] = 0; s.b[2] = (float)(bz - (double)s.origin[2]);
            s.c[0] = (float)(cx - (double)s.origin[0]); s.c[1] = 0; s.c[2] = (float)(cz - (double)s.origin[2]);
            m_surfaces.push_back(s);
        };
        tri(x0, z0, x1, z0, x1, z1);
        tri(x0, z0, x1, z1, x0, z1);
        // rigid bodies over the emitter field (anchor space x 0..96, z 0..48, emitters at y = 2)
        for (uint32_t b = 0; b < m_c.bodies; ++b)
        {
            NV_StreamSurface s{};
            s.body = b;
            s.entity[0] = 100000 + b;
            s.generation0 = 1;
            s.kind = 0; s.radius = 0.3f;                     // sphere at the body origin
            m_surfaces.push_back(s);
            s = NV_StreamSurface{}; s.body = b; s.entity[0] = 100000 + b; s.generation0 = 1;
            s.kind = 1; s.radius = 0.12f;                    // capsule beside it
            s.a[0] = 0.5f; s.a[1] = -0.3f; s.b[0] = 0.5f; s.b[1] = 0.3f;
            m_surfaces.push_back(s);
            s = NV_StreamSurface{}; s.body = b; s.entity[0] = 100000 + b; s.generation0 = 1;
            s.kind = 2;                                      // plate: two triangles, 0.8 m square, tilted
            const float px[4] = { -0.4f, 0.4f, 0.4f, -0.4f }, pz[4] = { -0.4f, -0.4f, 0.4f, 0.4f };
            auto corner = [&](float* out, int k) { out[0] = px[k] - 0.6f; out[1] = 0.2f * px[k]; out[2] = pz[k]; };
            corner(s.a, 0); corner(s.b, 1); corner(s.c, 2);
            m_surfaces.push_back(s);
            corner(s.a, 0); corner(s.b, 2); corner(s.c, 3);
            m_surfaces.push_back(s);
        }
    }

    // Deformable surfaces written in anchor space every tick (soft bodies, ropes): a waving 8 x 8 m sheet of 32
    // triangles over the middle of the emitter field, with the sheet's local velocity.
    std::vector<NV_StreamSurface> dynamicSurfaces(double t) const
    {
        std::vector<NV_StreamSurface> out;
        auto point = [&](int i, int j, double* p, double* v) {
            const double x = 40.0 + 2.0 * i, z = 16.0 + 2.0 * j, w = 1.7, k = 0.35;
            p[0] = x - m_c.anchorShift[0]; p[1] = 2.8 + 0.4 * std::sin(k * x + w * t) - m_c.anchorShift[1]; p[2] = z - m_c.anchorShift[2];
            v[0] = 0; v[1] = 0.4 * w * std::cos(k * x + w * t); v[2] = 0;
        };
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int half = 0; half < 2; ++half)
                {
                    NV_StreamSurface s{};
                    s.kind = 2;
                    s.body = NV_STREAM_NONE;
                    s.entity[0] = 200000;
                    s.generation0 = 1;
                    // origin = centroid (anchor space), offsets formed in double (NativeVfxStream.h)
                    double pa[3], pb[3], pc[3], va[3], vb[3], vc[3];
                    point(i, j, pa, va);
                    if (half == 0) { point(i + 1, j, pb, vb); point(i + 1, j + 1, pc, vc); }
                    else { point(i + 1, j + 1, pb, vb); point(i, j + 1, pc, vc); }
                    for (int a = 0; a < 3; ++a)
                    {
                        s.velocity[a] = (float)((va[a] + vb[a] + vc[a]) / 3);
                        s.origin[a] = (float)((pa[a] + pb[a] + pc[a]) / 3);
                        s.a[a] = (float)(pa[a] - (double)s.origin[a]);
                        s.b[a] = (float)(pb[a] - (double)s.origin[a]);
                        s.c[a] = (float)(pc[a] - (double)s.origin[a]);
                    }
                    out.push_back(s);
                }
        return out;
    }

    // Body b: a slow orbit around its home point and a spin about y (anchor space).
    std::vector<NV_StreamBody> bodyFrames(double t) const
    {
        std::vector<NV_StreamBody> out(m_c.bodies);
        for (uint32_t b = 0; b < m_c.bodies; ++b)
        {
            const double hx = 1.5 + 2.8 * (b % 36) - m_c.anchorShift[0], hz = 1.0 + 2.9 * ((b / 36) % 16) - m_c.anchorShift[2],
                         hy = 2.5 + 1.2 * (b % 3) - m_c.anchorShift[1];
            const double w = 0.4 + 0.05 * (b % 7), r = 0.6, spin = 0.8 + 0.1 * (b % 5), phase = 0.37 * b;
            NV_StreamBody& f = out[b];
            f.position[0] = (float)(hx + r * std::cos(w * t + phase));
            f.position[1] = (float)(hy + 0.3 * std::sin(1.3 * w * t + phase));
            f.position[2] = (float)(hz + r * std::sin(w * t + phase));
            f.velocity[0] = (float)(-r * w * std::sin(w * t + phase));
            f.velocity[1] = (float)(0.3 * 1.3 * w * std::cos(1.3 * w * t + phase));
            f.velocity[2] = (float)(r * w * std::cos(w * t + phase));
            const double a = spin * t + phase;
            f.rotation[0] = 0; f.rotation[1] = (float)std::sin(0.5 * a); f.rotation[2] = 0; f.rotation[3] = (float)std::cos(0.5 * a);
            f.angular_velocity[1] = (float)spin;
            // centre of mass 0.1 m off the body origin (body space x), so the rotation adds to the surface velocity
            f.center[0] = f.position[0] + (float)(0.1 * std::cos(a)); f.center[1] = f.position[1]; f.center[2] = f.position[2] - (float)(0.1 * std::sin(a));
        }
        return out;
    }

    RppConfig m_c;
    double m_anchor[3];
    uint64_t m_tick = 0;
    std::vector<NV_StreamEmitter> m_sent;  // whole table as the GPU holds it (last sent block of each row)
    std::vector<uint32_t> m_freeRows;      // inactive child rows (min-heap)
    uint32_t m_sentBlocks = 0;
    uint32_t m_capacity = 0, m_aliveAfter = 0, m_eventSlots = 0, m_childRows = 0, m_maxDepth = 0, m_capacityChanges = 0;
    std::vector<Program> m_programs;
    std::vector<NV_StreamCurveKey> m_keys;
    std::vector<NV_StreamField> m_fields;
    std::vector<NV_StreamWorldField> m_world;
    std::vector<NV_StreamSurface> m_surfaces;
    std::vector<Row> m_rows;
    std::vector<EventTime> m_events;
    std::vector<uint32_t> m_created;
};
} // namespace unx::fx::test
