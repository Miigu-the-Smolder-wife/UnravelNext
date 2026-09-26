// unx-kernel: cs_6_6 main
// Debug draw arguments (E, A15): one thread turns the appended counts into DispatchMesh arguments (32 primitives per
// group, capped at the capacities; appends past a capacity already set their status bit). P[0].x primitives UAV.
#include "Passes/Debug/DebugDraw.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer buf = ResourceDescriptorHeap[P[0].x];
    const uint lines = min(buf.Load(DEBUG_H_LINES), buf.Load(DEBUG_H_LINE_CAPACITY));
    const uint triangles = min(buf.Load(DEBUG_H_TRIANGLES), buf.Load(DEBUG_H_TRIANGLE_CAPACITY));
    const uint glyphs = min(buf.Load(DEBUG_H_GLYPHS), buf.Load(DEBUG_H_GLYPH_CAPACITY));
    buf.Store3(DEBUG_H_ARGS_LINES, uint3((lines + DEBUG_PER_GROUP - 1) / DEBUG_PER_GROUP, 1, 1));
    buf.Store3(DEBUG_H_ARGS_TRIANGLES, uint3((triangles + DEBUG_PER_GROUP - 1) / DEBUG_PER_GROUP, 1, 1));
    buf.Store3(DEBUG_H_ARGS_GLYPHS, uint3((glyphs + DEBUG_PER_GROUP - 1) / DEBUG_PER_GROUP, 1, 1));
}
