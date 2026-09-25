// unx-kernel: ps_6_6 main
// Pixel kernel of the VSM page raster through V's depth raster service (INTERFACES 5.3): one raster view per clipmap
// level, viewport = the level's 16384^2 window. Writes the caster height into the physical page of a dirty slot with
// InterlockedMax (the surface nearest the sun wins); pixels of pages that are not dirty write nothing. It also records,
// per page, whether wind-affected casters were drawn and their largest displacement bound (wind dirty rule, VsmRelease).
// userData: level (bits 0-4) | window origin x mod 128 (bits 5-11) | origin y mod 128 (bits 12-18)
// P[4].x pool UAV (raw), P[4].y page table SRV (raw), P[4].z hMin (float bits), P[4].w hMax (float bits)
// P[5].x pool pages per row, P[5].y page metadata UAV (VsmPageMeta)
#include "Passes/Visibility/DepthRaster.hlsli"
#include "Deformation.hlsli"
#include "Passes/Shadow/VsmCommon.hlsli"

void main(DepthRasterPixel p)
{
    if (!depthRasterCovered(p)) discard;
    const uint level = p.userData & 31u;
    const uint2 origin = uint2((p.userData >> 5) & 127u, (p.userData >> 12) & 127u);
    const uint2 px = uint2(p.position.xy);
    const uint2 slot2 = ((px >> VSM_PAGE_SHIFT) + origin) & (VSM_TABLE - 1);
    ByteAddressBuffer table = ResourceDescriptorHeap[P[4].y];
    const uint e = table.Load((level * VSM_SLOTS_PER_LEVEL + slot2.y * VSM_TABLE + slot2.x) * 8);
    if ((e & (VSM_FLAG_RESIDENT | VSM_FLAG_DIRTY)) != (VSM_FLAG_RESIDENT | VSM_FLAG_DIRTY)) return;
    const float hMin = asfloat(P[4].z), hMax = asfloat(P[4].w);
    const float h = hMax - p.position.z * (hMax - hMin);
    const uint phys = e & VSM_PHYS_MASK;
    RWByteAddressBuffer pool = ResourceDescriptorHeap[P[4].x];
    pool.InterlockedMax(vsmPoolAddress(phys, px & (VSM_PAGE - 1)), vsmEncode(h));
    const GpuInstance inst = loadInstance(p.instance);
    if ((inst.flags & INSTANCE_WIND) != 0)
    {
        RWStructuredBuffer<VsmPageMeta> meta = ResourceDescriptorHeap[P[5].y];
        const GpuMesh mesh = loadMesh(inst.mesh);
        const float amplitude = windOffsetBound(inst, mesh.boundsSphere.xyz, mesh.boundsSphere.w) * length(inst.objectToWorld[0].xyz);
        InterlockedMax(meta[phys].windAmplitude, asuint(amplitude));  // non-negative floats order as uints
        InterlockedMax(meta[phys].windCaster, 1u);
    }
}
