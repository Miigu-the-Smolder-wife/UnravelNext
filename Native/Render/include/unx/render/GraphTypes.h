#pragma once
// Render graph handles and resource descriptions (INTERFACES_KO.md 4): what frame contracts (ViewDesc, ViewResources,
// FrameResources, DepthRasterRequest) name. Split from RenderGraph.h (v1.42, infra request 20260926_Infra_header_split)
// so headers that only carry handles do not recompile with the graph's classes.
#include "unx/render/D3D12.h"

#include <cstdint>

namespace unx::render
{
// How a pass touches a resource. Write uses (Uav*, RenderTarget, DepthWrite, CopyDst, AccelerationStructureWrite,
// AccelerationStructureScratch) are read-modify-write.
// UavComputeDisjoint: the pass writes a region no other pass of the same run touches (tile-classified per-class
// material/shading dispatches, per-cascade ranges). Consecutive disjoint writers of a resource need no barrier between
// them; the first one still waits for earlier accesses and the next other access waits for all of them.
// SrvGraphics/UavGraphics synchronise with every shader stage (D3D12_BARRIER_SYNC_ALL_SHADING), which includes ray
// tracing shaders: DispatchRays passes declare their textures and buffers with them.
// AccelerationStructure*: buffers only, no views (the owner creates the AS SRV). The result buffers are created by
// their owner with D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE and imported; a pass may declare Write and
// Read of the same AS buffer (in-place refit).
enum class Use : uint8_t
{
    SrvCompute,
    SrvGraphics,
    UavCompute,
    UavComputeDisjoint,
    UavGraphics,
    RenderTarget,
    DepthWrite,
    DepthRead,
    IndirectArgs,
    CopySrc,
    CopyDst,
    AccelerationStructureWrite,    // build / refit destination (BLAS, TLAS)
    AccelerationStructureRead,     // traversal (DispatchRays, RayQuery), BLAS read by a TLAS build, refit source
    AccelerationStructureInput,    // build inputs: deformed vertices, index buffers, instance descriptors
    AccelerationStructureScratch,  // build scratch (transients get ALLOW_UNORDERED_ACCESS)
};

struct TextureDesc
{
    const char* name = "";
    uint32_t width = 1;
    uint32_t height = 1;
    uint16_t depthOrArraySize = 1;
    uint16_t mipLevels = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    // View formats other than 'format' (INTERFACES_KO.md 4, v1.13): the texture is created castable to them (relaxed
    // format casting, same bits per texel), e.g. written as R32_UINT through its UAV and filtered as
    // R9G9B9E5_SHAREDEXP through its SRV. UNKNOWN = 'format'.
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uavFormat = DXGI_FORMAT_UNKNOWN;
    // Creation-time optimization for transient render targets; this does NOT
    // initialize the texture. Passes still clear it explicitly.
    float clearColor[4] = {};
    float clearDepth = 0;  // reversed-Z default; source-triangle card captures use 1
};

struct BufferDesc
{
    const char* name = "";
    uint64_t size = 0;
    uint32_t stride = 0;  // 0 = raw (ByteAddressBuffer) views
};

struct TextureRef
{
    uint32_t id = UINT32_MAX;
    bool valid() const { return id != UINT32_MAX; }
};
struct BufferRef
{
    uint32_t id = UINT32_MAX;
    bool valid() const { return id != UINT32_MAX; }
};
} // namespace unx::render
