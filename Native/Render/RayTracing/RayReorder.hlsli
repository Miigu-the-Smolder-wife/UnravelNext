#ifndef UNX_RAY_REORDER_HLSLI
#define UNX_RAY_REORDER_HLSLI
#if defined(REORDER) && REORDER
#define NV_SHADER_EXTN_SLOT u0
#define NV_SHADER_EXTN_REGISTER_SPACE space31
#include "../../../External/nvapi/nvHLSLExtns.h"
#endif

void rtReorderHit(RtHit hit, RayDesc ray)
{
#if defined(REORDER) && REORDER == 2
    // The expensive hit lighting walks a spatial light list and surface-cache
    // pages. Instance-sized buckets fragmented those walks into small cohorts.
    // Four-metre hit cells keep nearby hits together without changing any ray,
    // material, random seed, or per-thread arithmetic.
    uint hint = 1023u;
    if (hit.t >= 0)
    {
        const int3 cell = int3(floor((ray.Origin + ray.Direction * hit.t) * 0.25));
        hint = (asuint(cell.x) * 73856093u ^ asuint(cell.y) * 19349663u ^ asuint(cell.z) * 83492791u) % 1020u;
        if (hit.instance == RT_INSTANCE_EMITTER) hint = 1022u;
        else if (hit.instance == RT_INSTANCE_FAR) hint = 1021u;
        else if (hit.instance >= 0xFFFF00u) hint = 1020u;
    }
    NvReorderThread(hint, 10);
#elif defined(REORDER) && REORDER
    // Group the material work by the hit's instance/submesh. Misses share one
    // hint. The hint affects scheduling only, never light/ray/sample selection.
    NvReorderThread(hit.t >= 0 ? (hit.instance ^ (hit.geometry * 1664525u)) : 0xFFFFu, 16);
#endif
}
#endif
