#include "../StreamFrustum.h"
#include "unx/core/Log.h"
#include "unx/scene/SceneData.h"
#include <limits>

using namespace unx;
using namespace unx::render;
using namespace unx::visibility::detail;
int main()
{
    try
    {
        scene::Camera camera;
        camera.position = {0, 0, 0}; camera.forward = {0, 0, 1};
        const auto view = ViewDesc::fromCamera(camera, 1920, 1080, {});
        auto checkVisible = [&](float3 lo, float3 hi, bool expected) {
            TriangleStream stream; stream.boundsMin = lo; stream.boundsMax = hi;
            if (streamInView(stream, view) != expected) fail("stream bounds visibility mismatch");
        };
        checkVisible({-1, -1, 2}, {1, 1, 4}, true);
        checkVisible({100, -1, 2}, {101, 1, 4}, false);
        checkVisible({-101, -1, 2}, {-100, 1, 4}, false);
        checkVisible({-1, 100, 2}, {1, 101, 4}, false);
        checkVisible({-1, -1, -4}, {1, 1, -2}, false);
        checkVisible({-10, -10, -1}, {10, 10, 1}, true);
        checkVisible({}, {}, true); // an external producer with no bounds
        checkVisible({NAN, -1, 2}, {1, 1, 4}, true);
        // Sweep through both horizontal edges. A visible corner is sufficient
        // to retain the whole AABB, including the two-pixel coverage guard.
        size_t checked = 0;
        for (int step = -5000; step <= 5000; ++step)
        {
            const float x = step * 0.01f;
            TriangleStream stream;
            stream.boundsMin = {x, -0.2f, 2}; stream.boundsMax = {x + 0.01f, 0.2f, 2.01f};
            const double cx = view.viewProj.m[0][0] * x + view.viewProj.m[0][2] * 2;
            const double cw = view.viewProj.m[3][0] * x + view.viewProj.m[3][2] * 2;
            if (std::abs(cx) <= cw * (1 + 4.0 / view.width))
            {
                if (!streamInView(stream, view)) fail("horizontal sweep lost a visible corner at %g", x);
                ++checked;
            }
        }
        logf("PASS stream bounds: %zu horizontal edge positions plus inside/outside/behind/camera-crossing/unknown bounds\n", checked);
        return 0;
    }
    catch (const std::exception& error) { logf("FAIL %s\n", error.what()); return 1; }
}
