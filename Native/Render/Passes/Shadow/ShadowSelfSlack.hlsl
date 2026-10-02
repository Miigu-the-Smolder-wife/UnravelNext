// unx-kernel: cs_6_6 main
// Receivers that take no shadow from their own instance (scene::InstanceNoSelfShadow; S records this pass in the frames
// whose scene has such an instance). Per pixel of the view: 0, or - where the visible surface belongs to such an instance -
// the diameter of the instance's bounding sphere. The sun's lookups of the pixel (ShadowVisibility.hlsl,
// ShadowPenumbra.hlsl) start that far toward the sun, past every caster of the instance itself; the casters between
// that point and the sun shade the pixel as before. What it leaves out with them: other casters inside the instance's
// bounds along the sun's direction, and the screen-space contact ray (it meets the instance's own depth).
// P[0] = { vis id SRV (R32_UINT), visible clusters SRV, slack UAV (R16_FLOAT), 0 }. Frame constants of the view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "VisBuffer.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_viewWidth || id.y >= g_viewHeight) return;
    Texture2D<uint> vis = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float> slack = ResourceDescriptorHeap[P[0].z];
    const uint v = vis.Load(int3(id.xy, 0));
    float s = 0;
    if (v != VIS_NONE)
    {
        const GpuInstance inst = loadInstance(loadVisibleCluster(P[0].y, visVisibleCluster(v)).instance);
        if ((inst.flags & INSTANCE_NO_SELF_SHADOW) != 0) s = 2 * loadMesh(inst.mesh).boundsSphere.w * length(inst.objectToWorld[0].xyz);
    }
    slack[id.xy] = s;
}
