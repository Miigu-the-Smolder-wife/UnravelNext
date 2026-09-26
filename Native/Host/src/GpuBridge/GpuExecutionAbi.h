#pragma once
#include <stdint.h>
#if defined(_WIN32)
#define NRC_GPU_CALL __cdecl
#else
#define NRC_GPU_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Runtime-only ABI. Fence values are local to one device/queue timeline. */
enum NRC_GpuResult { NRC_GPU_OK=0,NRC_GPU_INVALID=1,NRC_GPU_STALE=2,NRC_GPU_PRESSURE=3,NRC_GPU_REMOVED=4,NRC_GPU_PENDING=5,NRC_GPU_INTERNAL=6 };
enum NRC_GpuQueue { NRC_GPU_GRAPHICS=0,NRC_GPU_COMPUTE=1,NRC_GPU_COPY=2,NRC_GPU_QUEUE_COUNT=3 };
enum NRC_GpuStage { NRC_GPU_UPLOAD=0,NRC_GPU_PHYSICS=1,NRC_GPU_VFX=2,NRC_GPU_DEFORMATION=3,NRC_GPU_RTAS=4,NRC_GPU_TRANSPORT=5,NRC_GPU_RECONSTRUCTION=6,NRC_GPU_PRESENTATION=7 };
enum NRC_GpuDomain { NRC_GPU_RENDERER=0,NRC_GPU_PHYSICS_DOMAIN=1,NRC_GPU_VFX_DOMAIN=2,NRC_GPU_ANIMATION_DOMAIN=3,NRC_GPU_DOMAIN_COUNT=4 };
enum NRC_GpuMemory { NRC_GPU_PERSISTENT=0,NRC_GPU_STREAMING=1,NRC_GPU_TRANSIENT=2,NRC_GPU_RTAS_MEMORY=3,NRC_GPU_HISTORY=4,NRC_GPU_UPLOAD_MEMORY=5,NRC_GPU_READBACK=6,NRC_GPU_MEMORY_COUNT=7 };
enum NRC_GpuAccessMode { NRC_GPU_READ=1,NRC_GPU_WRITE=2 };
typedef struct NRC_GpuFence {uint64_t generation;uint32_t queue,reserved;uint64_t value;} NRC_GpuFence;
typedef struct NRC_GpuAllocation {
    uint32_t size,version;
    uint64_t bytes,owner;
    uint32_t domain,memory,nonlocal,reserved;
} NRC_GpuAllocation;
typedef struct NRC_GpuAccess {
    uint64_t resource,version;
    uint32_t before_state,after_state,mode,reserved;
} NRC_GpuAccess;
typedef struct NRC_GpuWorldStamp {
    uint64_t world,world_generation,epoch,tick,branch;
    uint32_t phase,reserved;
} NRC_GpuWorldStamp;
typedef struct NRC_GpuSubmission {
    uint32_t size,version,queue,stage;
    NRC_GpuWorldStamp source;
    void* commands;void* command_allocator;
    const NRC_GpuAccess* accesses;uint64_t access_count;
    const NRC_GpuFence* waits;uint64_t wait_count;
    void* const* lifetime_objects;uint64_t lifetime_count;
} NRC_GpuSubmission;
typedef struct NRC_GpuStatistics {
    uint32_t size,version;
    uint64_t generation,submitted[3],completed[3],submissions[3],queue_waits,cpu_waits;
    uint64_t resources,reserved_bytes,bound_bytes,retiring_bytes,denied_allocations;
    uint64_t domain_bytes[NRC_GPU_DOMAIN_COUNT],memory_bytes[NRC_GPU_MEMORY_COUNT];
    int32_t last_error;uint32_t faulted;
} NRC_GpuStatistics;
/* Reserve starts a CPU lease. Consumers retain before producer release.
   Last CPU release prevents new work, but backing resources survive all GPU
   consumers. Internal barriers are recorded by the producer. Boundary states
   are checked, and waits may only reference already-submitted work.
   before_state is the actual entry state, including the creation state on a
   first submission. after_state is the list's final state; the D3D12 backend
   applies automatic decay to the next boundary. Published buffer views must
   expose that post-submission state, not an earlier in-list read state.
   Graphics submission belongs to Unity, not this cross-module callback table. */
typedef struct NRC_GpuBridge {
    uint32_t size,version;uint64_t generation;
    void *context,*device,*compute_queue,*compute_fence,*copy_queue,*copy_fence;
    int32_t (NRC_GPU_CALL *retain)(void*);
    void (NRC_GPU_CALL *release)(void*);
    int32_t (NRC_GPU_CALL *reserve)(void*,const NRC_GpuAllocation*,uint64_t*);
    int32_t (NRC_GPU_CALL *bind)(void*,uint64_t,void*,uint32_t);
    int32_t (NRC_GPU_CALL *retain_resource)(void*,uint64_t);
    int32_t (NRC_GPU_CALL *release_resource)(void*,uint64_t);
    int32_t (NRC_GPU_CALL *submit)(void*,const NRC_GpuSubmission*,NRC_GpuFence*);
    int32_t (NRC_GPU_CALL *poll)(void*,NRC_GpuFence,uint32_t,uint32_t*);
    int32_t (NRC_GPU_CALL *statistics)(void*,NRC_GpuStatistics*);
} NRC_GpuBridge;
#ifdef __cplusplus
}
static_assert(sizeof(NRC_GpuWorldStamp)==48);
static_assert(sizeof(NRC_GpuFence)==24&&sizeof(NRC_GpuAllocation)==40&&sizeof(NRC_GpuAccess)==32);
static_assert(sizeof(NRC_GpuSubmission)==128&&sizeof(NRC_GpuBridge)==136);
#endif
