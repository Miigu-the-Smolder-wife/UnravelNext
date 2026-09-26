// Mesh particle orientation in the FX stream executor (NativeVfx stream executor version 4, NV_STREAM_EXECUTOR_MESH_ORIENTATION;
// owner: engine 2). Slots of programs with FX_PROGRAM_ORIENTATION carry NV_StreamParticleOrientation (32 B: rotation
// quaternion (x, y, z, w) local -> anchor frame, spin (rad/s, anchor frame)) in a buffer pair laid out like posAge: the
// input at the last tick's layout index, the output at this tick's. The rules are VfxParticleMath's (the CPU reference
// executor, VfxStreamCpu.h, in the same order):
//   birth    fxOrientBirth  (FxSpawn place, before fxStep): nv_orientation_birth, then advanced over the birth's elapsed time;
//   existing fxOrientCarry  (FxIntegrate, after rebase / transport, for a slot that lives on): the transport's rotation
//            turns rotation and spin, then advanced over dt with the spin before the tick; a state packet copies;
//   impact   fxOrientImpact (fxFinishSlot, when the slot's sweep had an impact this tick): spin *= 1 - friction.
// Buffers: FxTick g_orientationIn / g_orientationOut (0 until a program of the stream carries an orientation: every hook
// returns), g_restoreOrientations (a RESET with NV_STREAM_ORIENTATION: FxBegin installs them with the restore records).
// The program's mesh fields are StreamProgram.reserved6 (NV_StreamProgram mesh_asset[2], spin_min, spin_max).
#ifndef FX_MESH_ORIENTATION_HLSLI
#define FX_MESH_ORIENTATION_HLSLI

#define FX_PROGRAM_ORIENTATION 256u
#define FX_ORIENTATION_IN g_orientationIn
#define FX_ORIENTATION_OUT g_orientationOut

struct ParticleOrientation { float4 rotation; float3 spin; float reserved0; };  // 32 B (NV_StreamParticleOrientation)

bool fxOriented(RowMotion rm) { return FX_ORIENTATION_OUT != 0u && (fxRowProgramFlags(rm) & FX_PROGRAM_ORIENTATION) != 0u; }

// A new birth of 'row' written at this tick's layout index: its birth orientation advanced over 'elapsed'.
void fxOrientBirth(uint index, uint row, uint birth, RowMotion rm, float elapsed)
{
    if (!fxOriented(rm)) return;
    FX_RWBUFFER(StreamEmitter, emitters, g_emitters);
    FX_BUFFER(StreamProgram, programs, g_programs);
    const StreamEmitter e = emitters[row];
    const StreamProgram p = programs[e.program];
    ParticleOrientation o;
    nv_orientation_birth(nv_birth_rng(e.rngKey, birth), asfloat(p.reserved6.z), asfloat(p.reserved6.w), o.rotation, o.spin);
    o.rotation = nv_orientation_advance(o.rotation, o.spin, elapsed);
    o.reserved0 = 0;
    FX_RWBUFFER(ParticleOrientation, turnsOut, FX_ORIENTATION_OUT);
    turnsOut[index] = o;
}

// An existing slot (input index i) that lives on at output index 'index': transported (rows = the emitter's transport when
// 'transported'), then advanced over h (0: a state packet copies).
void fxOrientCarry(uint i, uint index, RowMotion rm, bool transported, float4 t0, float4 t1, float4 t2, float h)
{
    if (!fxOriented(rm)) return;
    FX_RWBUFFER(ParticleOrientation, turnsIn, FX_ORIENTATION_IN);
    ParticleOrientation o = turnsIn[i];
    if (transported)
    {
        const float4 q = nv_quat_from_rows(t0, t1, t2);
        o.rotation = nv_quat_normalize(nv_quat_mul(q, o.rotation));
        o.spin = nv_quat_rotate(q, o.spin);
    }
    if (h > 0) o.rotation = nv_orientation_advance(o.rotation, o.spin, h);
    FX_RWBUFFER(ParticleOrientation, turnsOut, FX_ORIENTATION_OUT);
    turnsOut[index] = o;
}

// After the slot's tick, when its sweep had an impact: the contacts damp the spin (a resting slot's spin decays to rest).
void fxOrientImpact(uint index, RowMotion rm, uint impacts)
{
    if (impacts == 0u || !fxOriented(rm)) return;
    FX_RWBUFFER(ParticleOrientation, turnsOut, FX_ORIENTATION_OUT);
    turnsOut[index].spin *= 1.0f - rm.friction;
}
// RESET: restore record j (installed at input index 'index') brings its orientation (NV_STREAM_ORIENTATION).
void fxOrientRestore(uint j, uint index)
{
    if (g_restoreOrientations == 0u || FX_ORIENTATION_IN == 0u) return;
    FX_BUFFER(ParticleOrientation, restored, g_restoreOrientations);
    FX_RWBUFFER(ParticleOrientation, turnsIn, FX_ORIENTATION_IN);
    turnsIn[index] = restored[j];
}
#endif
