// unx-kernel: ps_6_6 main
// Pixel kernel of the local-light page raster through V's depth raster service (INTERFACES 5.3): one raster view per
// (light, cube face, mip), all in one 16384^2 viewport whose top-left res x res pixels are the mip (VsmSystem.cpp).
// Writes vsmEncode(-z) of the caster's face depth into the physical page of a dirty slot with InterlockedMax (the caster
// nearest the light wins); pixels of pages that are not dirty write nothing. Records wind casters per page (VsmRelease).
// userData: shadow slot (bits 0-6) | face (bits 7-9) | mip (bits 10-12)
// P[4].x pool UAV (raw), P[4].y page table SRV (raw), P[4].z local lights SRV (StructuredBuffer<VsmLocalLight>)
// P[5].x unused, P[5].y page metadata UAV (VsmPageMeta)
#include "Passes/Visibility/DepthRaster.hlsli"
#include "Deformation.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"

void main(DepthRasterPixel p)
{
    if (!depthRasterCovered(p)) discard;
    const uint light = p.userData & 127u, face = (p.userData >> 7) & 7u, mip = (p.userData >> 10) & 7u;
    const uint2 px = uint2(p.position.xy);
    const uint2 page = px >> VSM_PAGE_SHIFT;
    if (any(page >= (1u << mip))) return;
    ByteAddressBuffer table = ResourceDescriptorHeap[P[4].y];
    const uint slot = vsmLocalSlot(light, face, mip, page);
    const uint e = table.Load(slot * 8);
    if ((e & (VSM_FLAG_RESIDENT | VSM_FLAG_DIRTY)) != (VSM_FLAG_RESIDENT | VSM_FLAG_DIRTY)) return;
    StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[4].z];
    const VsmLocalLight l = lights[light];
    // Device depth d = f (z - n) / ((f - n) z)  ->  z = f n / (f - d (f - n)).
    const float z = l.farM * l.nearM / (l.farM - p.position.z * (l.farM - l.nearM));
    const uint phys = e & VSM_PHYS_MASK;
    RWByteAddressBuffer pool = ResourceDescriptorHeap[P[4].x];
    pool.InterlockedMax(vsmPoolAddress(phys, px & (VSM_PAGE - 1)), vsmEncode(-z));
    const GpuInstance inst = loadInstance(p.instance);
    if ((inst.flags & INSTANCE_WIND) != 0)
    {
        RWStructuredBuffer<VsmPageMeta> meta = ResourceDescriptorHeap[P[5].y];
        const GpuMesh mesh = loadMesh(inst.mesh);
        const float amplitude = windOffsetBound(inst, mesh.boundsSphere.xyz, mesh.boundsSphere.w) * length(inst.objectToWorld[0].xyz);
        InterlockedMax(meta[phys].windAmplitude, asuint(amplitude));
        InterlockedMax(meta[phys].windCaster, 1u);
    }
}
