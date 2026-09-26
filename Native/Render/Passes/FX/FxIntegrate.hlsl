// unx-kernel: cs_6_6 main
// Integrate of the existing particles (stream step 2): thread per particle of the input layout (the last tick's output:
// per row its live births in birth order, InRange; Particles.hlsli "Particle layout"). New births are integrated by
// FxSpawn at their depth. Per particle, in the stream's order: a particle of a KILLED row vanishes (no event); after
// a. rebase / transport, one whose birth is in the row's dying range [dying_birth, death_birth) dies, integrated to its
// lifetime end first when the row writes death events (event slot death_event + (birth - dying_birth)); every other one
// runs b-f over h = dt with the row's full-dt drag factors (fxStep) and writes this tick's state at its output index
// outBase + (birth - outFirst) in the other buffer of the pair (the renderer interpolates the two ticks of a particle from
// the state itself). A state packet (dt == 0) only carries the state over (and applies KILLED).
#define FX_FINISH_SWEEP 0  // colliding slots go to FxCollide (fxFinishSlot)
#include "Passes/FX/Particles.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    const uint i = id.x;
    if (i >= g_inCount) return;
    const InRange range = fxInRange(i, gid.x);
    const uint row = range.row, birth = range.first + (i - range.base);
    if (row >= g_emitterCount) { fxStatus(FX_STATUS_RANGE); return; }
    FX_RWBUFFER(RowMotion, rowMotion, g_rowMotion);
    const RowMotion rm = rowMotion[row];
    const uint ef = fxRowEmitterFlags(rm);
    // A KILLED row's particles vanish (no event, no dying range; the row has no output range).
    if ((ef & FX_EMITTER_KILLED) != 0u) return;
    FX_RWBUFFER(float4, posAge, g_posAge);
    FX_RWBUFFER(float4, velocity, g_velocity);
    FX_RWBUFFER(EmitterDynamic, dynamic, g_emitterDynamic);
    const float4 pa = posAge[i];
    const float4 vv = velocity[i];
    const EmitterDynamic dyn = dynamic[row];
    NvState s;
    s.position = pa.xyz; s.velocity = vv.xyz; s.age = pa.w;
    const uint rank = birth - range.outFirst, index = range.outBase + rank;
    if (g_dt == 0)
    {
        // State packet (restore, same-tick population change): no motion; the output state is the state.
        if (rank >= range.outCount || index >= g_outCount) { fxStatus(FX_STATUS_RANGE); return; }
        FX_RWBUFFER(float4, posAgeOut, g_posAgeOut);
        FX_RWBUFFER(float4, velocityOut, g_velocityOut);
        posAgeOut[index] = float4(s.position, s.age);
        velocityOut[index] = float4(s.velocity, 0);
        fxCountAlive();
        fxWriteOutputs(index, row, birth, s, rm, dyn);
        return;
    }
    // a. rebase, transport (every existing particle, dying ones too: event positions are in this tick's origin space)
    s.position -= rm.rebase;
    if ((ef & FX_EMITTER_TRANSPORT) != 0u)
    {
        FX_RWBUFFER(StreamEmitter, emitters, g_emitters);
        const StreamEmitter e = emitters[row];
        s.position = nv_affine_point(e.transport[0], e.transport[1], e.transport[2], s.position);
        s.velocity = nv_affine_vector(e.transport[0], e.transport[1], e.transport[2], s.velocity);
    }
    if (birth - rm.dyingBirth < rm.deathBirth - rm.dyingBirth)
    {
        if ((ef & FX_EMITTER_DEATH_EVENTS) != 0u && rm.deathEvent != FX_NONE)
        {
            const float rest = max(0.0f, rm.lifetime - s.age);
            NvImpact impact;
            nv_integrate(fxMotionRow(rm, dyn, birth), rest, nv_linear_drag(rm.drag, rest), s, impact);
            fxWriteEvent(rm.deathEvent + (birth - rm.dyingBirth), fxEvent(row, birth, FX_EVENT_DEATH, s));
        }
        return;
    }
    if (rank >= range.outCount || index >= g_outCount) { fxStatus(FX_STATUS_RANGE); return; }
    NvDrag drag;
    drag.velocity = rm.dragVelocity; drag.position = rm.dragPosition; drag.acceleration = rm.dragAcceleration;
    fxStep(index, row, birth, rm, dyn, s, g_dt, drag, false);
}
