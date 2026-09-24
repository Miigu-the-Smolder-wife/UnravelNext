#pragma once
#include <cstdint>
#include <functional>

namespace unx
{
// The process-wide worker pool (ARCHITECTURE_KO.md 5.1: one pool, no per-module thread pools). P0b needs only
// fork-join parallel loops (PSO creation, graph compilation); the phase DAG with declared read/write sets comes with
// World/URC in P6 and is built on the same workers.
class Jobs
{
public:
    static Jobs& instance();
    uint32_t workerCount() const;
    // Runs fn(i) for i in [0, count) on the workers and the calling thread; returns when all finished.
    // The first exception thrown by any fn is rethrown on the caller.
    void parallelFor(uint32_t count, const std::function<void(uint32_t)>& fn);

private:
    Jobs();
    ~Jobs();
    struct Impl;
    Impl* m_impl;
};
} // namespace unx
