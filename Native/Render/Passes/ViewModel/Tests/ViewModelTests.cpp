// Track E view model correctness (A12; FEATURES_GAME 3). A camera moving and turning over 6 frames, a view model (a
// scene instance) posed in the camera's frame:
//   - its world transform each frame = the frame's camera-to-world x its camera-local pose (1e-5);
//   - a vertex of it lands on the same pixel through (viewProj, objectToWorld) and (prevViewProj, prevObjectToWorld):
//     zero motion relative to the camera, so neither motion vectors nor the camera-rotation blur move it (2e-5 NDC);
//   - a camera cut (discontinuity) teleports it (prev = current);
//   - the GPU scene flag kInstanceViewModel is set on it and on nothing else, cleared on removal;
//   - a pose nearer than the near plane is counted (Stats::nearClipped);
//   - viewModelClip (ViewModel.hlsli) scales clip.xy of view-model instances only, by viewmodel.fov_override_degrees'
//     k = tan(fov / 2) / tan(fov_vm / 2) (fov 90, override 60: 1.7320508).
//   unx_test_viewmodel_viewmodeltests [--no-debug-layer]
#include "../../Atmosphere/Tests/TestFrame.h"

#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"
#include "unx/viewmodel/ViewModel.h"

#include <cmath>
#include <cstdio>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

namespace
{
float3x4 rotationY(float a, float3 t)
{
    float3x4 m;
    m.m[0][0] = std::cos(a); m.m[0][2] = std::sin(a); m.m[0][3] = t.x;
    m.m[1][1] = 1; m.m[1][3] = t.y;
    m.m[2][0] = -std::sin(a); m.m[2][2] = std::cos(a); m.m[2][3] = t.z;
    return m;
}
float3 rowsPoint(const float4 (&r)[3], float3 p)
{
    return { r[0].x * p.x + r[0].y * p.y + r[0].z * p.z + r[0].w, r[1].x * p.x + r[1].y * p.y + r[1].z * p.z + r[1].w,
             r[2].x * p.x + r[2].y * p.y + r[2].z * p.z + r[2].w };
}
float2 ndc(const float4x4& m, float3 p)
{
    const float x = m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3];
    const float y = m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3];
    const float w = m.m[3][0] * p.x + m.m[3][1] * p.y + m.m[3][2] * p.z + m.m[3][3];
    return { x / w, y / w };
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--no-debug-layer") debugLayer = false;
            else fail("unknown argument %s", a.c_str());
        }
        TestFrame tf(debugLayer);
        scene::Scene sc;
        sc.name = "view model test";
        scene::Mesh mesh;
        mesh.name = "hand";
        for (float3 q : { float3{ -0.05f, -0.05f, 0 }, float3{ 0.05f, -0.05f, 0 }, float3{ 0.05f, 0.05f, 0 }, float3{ -0.05f, 0.05f, 0 } })
        {
            mesh.positions.push_back(q);
            mesh.normals.push_back({ 0, 0, 1 });
            mesh.uv0.push_back({ q.x, q.y });
        }
        mesh.indices = { 0, 1, 2, 0, 2, 3 };
        mesh.submeshes.push_back({ 0, 6, 0 });
        sc.meshes.push_back(mesh);
        sc.materials.push_back({});
        for (int k = 0; k < 3; ++k)
        {
            scene::Instance in;
            in.flags = scene::InstanceCastShadow | scene::InstanceDynamic;
            sc.instances.push_back(in);
        }
        scene::Camera cam;
        cam.name = "main";
        cam.verticalFov = 3.14159265f / 2;
        sc.cameras.push_back(cam);
        tf.setScene(sc);

        viewmodel::ViewModels& set = viewmodel::viewModels(tf.trackState);
        float3x4 pose;  // 0.3 m forward, 0.1 m right and down, turned 20 degrees
        pose = rotationY(0.349f, { 0.1f, -0.1f, -0.3f });
        const uint32_t id = set.add(1, pose);
        float4x4 prevViewProj;
        double worstPlace = 0, worstMotion = 0;
        for (int f = 0; f < 6; ++f)
        {
            scene::Camera c = cam;
            c.position = { 0.4f * f, 1.7f + 0.05f * f, -0.3f * f };
            const float yaw = 0.25f * f, pitch = -0.1f * f;
            c.forward = normalize(float3{ std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch) });
            ViewDesc v = ViewDesc::fromCamera(c, 1280, 720, f == 0 ? float4x4{} : prevViewProj);
            if (f == 0 || f == 3) v.prevViewProj = v.viewProj;  // FrameRenderer: no previous view at a discontinuity
            tf.frame.mainView = v;
            tf.frame.discontinuity = f == 3 ? kDiscontinuityCut : 0u;
            tracks::viewModelPrepare(tf.trackState, tf.gpuScene, tf.quality, tf.frame);
            tf.gpuScene.flushUpdates(tf.frame.frameIndex, 2, tf.shaders);
            tf.run([&](FramePassContext&) {});
            const gpu::Instance& gi = tf.gpuScene.instances()[1];
            const float3x4 expect = viewmodel::cameraToWorld(v);
            const float3 corner{ 0.05f, 0.05f, 0 };
            const float3 got = rowsPoint(gi.objectToWorld, corner), want = expect.transformPoint(pose.transformPoint(corner));
            worstPlace = std::max(worstPlace, (double)length(got - want));
            S_CHECK(length(got - want) <= 1e-5f, "frame %d: view model corner %.2e m off camera x pose", f, length(got - want));
            if (f > 0)
            {
                const float2 now = ndc(v.viewProj, got), before = ndc(v.prevViewProj, rowsPoint(gi.prevObjectToWorld, corner));
                const float m = std::max(std::abs(now.x - before.x), std::abs(now.y - before.y));
                worstMotion = std::max(worstMotion, (double)m);
                S_CHECK(m <= 2e-5f, "frame %d: view model moved %.2e NDC relative to the camera", f, m);
            }
            if (f == 3) S_CHECK(std::memcmp(gi.prevObjectToWorld, gi.objectToWorld, sizeof gi.objectToWorld) == 0, "cut frame: no teleport");
            S_CHECK((gi.flags & gpu::kInstanceViewModel) && !(tf.gpuScene.instances()[0].flags & gpu::kInstanceViewModel) &&
                        !(tf.gpuScene.instances()[2].flags & gpu::kInstanceViewModel),
                    "frame %d: view-model flag on the wrong instances", f);
            prevViewProj = v.viewProj;
        }
        S_CHECK(viewmodel::lastStats(tf.trackState).viewModels == 1 && viewmodel::lastStats(tf.trackState).nearClipped == 0, "stats");
        std::printf("view model: 6 frames of a moving, turning camera - placed within %.1e m, motion relative to the camera %.1e NDC, cut teleports\n",
                    worstPlace, worstMotion);

        // near plane (0.05 m by default): a pose 1 cm in front of the camera reaches past it
        set.setPose(id, rotationY(0, { 0, 0, -0.01f }));
        tracks::viewModelPrepare(tf.trackState, tf.gpuScene, tf.quality, tf.frame);
        S_CHECK(viewmodel::lastStats(tf.trackState).nearClipped == 1, "a view model through the near plane was not counted");

        // projection remap: fov 90, override 60 -> k = tan 45 / tan 30
        tf.quality.applyOverride("viewmodel.fov_override_degrees=60");
        const float k = tracks::viewModelPrepare(tf.trackState, tf.gpuScene, tf.quality, tf.frame);
        S_CHECK(std::abs(k - 1.7320508f) <= 1e-5f && std::abs(viewmodel::lastStats(tf.trackState).scale - k) == 0, "k = %.7f", k);
        tf.gpuScene.flushUpdates(tf.frame.frameIndex, 2, tf.shaders);
        std::shared_ptr<std::vector<uint8_t>> clip;
        ComPtr<ID3D12Resource> ring = tf.makeBuffer(1024, D3D12_HEAP_TYPE_UPLOAD);
        tf.run([&](FramePassContext& fc) {
            gpu::FrameConstants c = FrameRenderer::frameConstants(tf.gpuScene, tf.frame, tf.frame.mainView);
            c.viewModelScale = k;
            void* p = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(ring->Map(0, &none, &p), "map");
            std::memcpy(p, &c, sizeof c);
            ring->Unmap(0, nullptr);
            const D3D12_GPU_VIRTUAL_ADDRESS cb = ring->GetGPUVirtualAddress();
            const BufferRef out = fc.graph.createBuffer(BufferDesc{ "viewmodel.test.clip", 3 * 16, 16 });
            ID3D12PipelineState* pso = fc.shaders.compute("Passes/ViewModel/Tests/ViewModelProbe");
            fc.graph.addPass("viewmodel.test.probe", QueueType::Graphics, [&](PassBuilder& b) { b.use(out, Use::UavCompute); },
                             [=](PassContext& ctx) {
                                 const uint32_t kk[4] = { ctx.uav(out), 0, 3, 0 };
                                 ctx.cmd->SetPipelineState(pso);
                                 ctx.bindFrameConstants(cb);
                                 ctx.computeConstants(kk, 4);
                                 ctx.cmd->Dispatch(1, 1, 1);
                             });
            clip = tf.readbackBuffer(fc, out, 3 * 16);
        });
        const float* r = reinterpret_cast<const float*>(clip->data());
        for (int i = 0; i < 3; ++i)
        {
            const float s = i == 1 ? k : 1.0f;
            S_CHECK(std::abs(r[4 * i] - 0.25f * s) <= 1e-6f && std::abs(r[4 * i + 1] + 0.5f * s) <= 1e-6f && r[4 * i + 2] == 0.01f && r[4 * i + 3] == 2.0f,
                    "instance %d: clip (%.6f, %.6f, %.6f, %.6f)", i, r[4 * i], r[4 * i + 1], r[4 * i + 2], r[4 * i + 3]);
        }
        tf.quality.applyOverride("viewmodel.fov_override_degrees=0");
        std::printf("view model: near-plane reach counted; remap k = %.7f scales clip.xy of the view model only (z, w kept)\n", k);

        // removal clears the flag
        set.remove(id);
        tracks::viewModelPrepare(tf.trackState, tf.gpuScene, tf.quality, tf.frame);
        S_CHECK(!(tf.gpuScene.instances()[1].flags & gpu::kInstanceViewModel), "removed view model keeps its flag");
        const uint32_t errors = tf.device.drainDebugMessages();
        S_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("view model tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
