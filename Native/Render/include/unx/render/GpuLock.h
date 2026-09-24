#pragma once
// GPU measurement lock (INTERFACES_KO.md 3.3). Performance measurements run one at a time across all sessions:
// Tools/CI/GpuLock.ps1 holds a named mutex while it runs the measuring command and sets UNX_GPU_LOCK=<track> for it.
// Correctness runs (tests, validation, reference comparisons) do not take the lock.
#include <string>

namespace unx::render
{
// Throws unless the process runs under GpuLock.ps1. Every performance tool (Harness::run, microbench-style
// experiments, gates) calls it before measuring. Returns the holder's track name.
std::string requireGpuLock(const char* what);
} // namespace unx::render
