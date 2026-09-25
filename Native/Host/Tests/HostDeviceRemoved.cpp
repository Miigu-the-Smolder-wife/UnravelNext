// Device removal inside a host (I track, INTERFACES v1.27): with DeviceRemovedPolicy::Throw, as the Unity plugin sets
// it, a removed device must not end the process. A standalone HostRenderer draws a few frames of a one-box scene, its
// device is removed (ID3D12Device5::RemoveDevice: this process only, no GPU reset, no TDR), and then:
//   - the next frame fails with render::DeviceRemovedError (the ABI maps it to UNX_DEVICE_REMOVED),
//   - deviceWasRemoved() reports the removal,
//   - destroying the renderer returns (its waits return on a removed device) and the process continues.
// Hardware GPU run: GpuLock -Track I while the temporary lock rule is in force.
#include "Renderer/HostRenderer.h"

#include "unx/core/File.h"
#include "unx/render/D3D12.h"
#include "unx/scene/SceneData.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace unx;
using namespace unx::host;

namespace
{
scene::Scene oneBox()
{
    scene::Scene s;
    s.name = "device removed";
    scene::Material m;
    m.name = "grey";
    s.materials.push_back(m);
    scene::Mesh mesh;
    mesh.name = "box";
    const float3 axes[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (const float3& a : axes)
    {
        const float3 u = a.x != 0 ? float3{ 0, 0, 1 } : float3{ 1, 0, 0 };
        const float3 w = cross(a, u);  // u x w = a
        const uint32_t base = (uint32_t)mesh.positions.size();
        const float su[4] = { -1, 1, 1, -1 }, sw[4] = { -1, -1, 1, 1 };
        for (int c = 0; c < 4; ++c)
        {
            mesh.positions.push_back((a + u * su[c] + w * sw[c]) * 0.5f);
            mesh.normals.push_back(a);
        }
        mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    mesh.submeshes.push_back({ 0, (uint32_t)mesh.indices.size(), 0 });
    s.meshes.push_back(std::move(mesh));
    scene::Instance inst;
    inst.mesh = 0;
    s.instances.push_back(inst);
    scene::Camera c;
    c.name = "front";
    c.position = { 0, 0.5f, 3 };
    s.cameras.push_back(c);
    return s;
}
} // namespace

int main()
{
    try
    {
        render::setDeviceRemovedPolicy(render::DeviceRemovedPolicy::Throw);
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        auto h = std::make_unique<HostRenderer>(o);
        h->scene() = oneBox();
        const scene::Camera camera = h->scene().cameras[0];
        h->commit();
        auto frame = [&](uint64_t index) {
            FramePacket p;
            p.frameIndex = index;
            p.deltaTime = 1.0f / 60;
            p.width = 2560;
            p.height = 1440;
            p.camera = camera;
            h->renderStandalone(h->queueFrame(std::move(p)), nullptr, 0);
        };
        for (uint64_t f = 0; f < 4; ++f) frame(f);
        if (render::deviceWasRemoved()) fail("device reported removed before the test removed it");
        h->removeDeviceForTest();
        bool thrown = false;
        std::string message;
        for (uint64_t f = 4; f < 8 && !thrown; ++f)
        {
            try
            {
                frame(f);
            }
            catch (const render::DeviceRemovedError& e)
            {
                thrown = true;
                message = e.what();
            }
        }
        if (!thrown) fail("no DeviceRemovedError within 4 frames after RemoveDevice");
        if (!render::deviceWasRemoved()) fail("deviceWasRemoved() is false after the error");
        h.reset();  // waits return on a removed device; must not throw or end the process
        logf("device removed: '%s'; renderer destroyed, process continues\n", message.c_str());
        logf("HOST DEVICE REMOVED TEST PASSED\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
