#pragma once
// tracks::pending (Tracks.h) on its own (v1.42): the core stubs' and unimplemented entries' log-once call, for files
// that need nothing else of the frame contract (src/GpuLock.cpp defines it).
namespace unx::render::tracks
{
// Entry points a track has not implemented yet call this: it logs once per entry and the entry declares no passes.
void pending(const char* entry);
} // namespace unx::render::tracks
