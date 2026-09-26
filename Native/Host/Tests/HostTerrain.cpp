// Terrain deformation through the host (render C, C5; INTERFACES v1.57): two cooked-format terrain tiles (32 x 32 cells
// of 0.5 m, side by side) and footprints in a 5 cm D window, one across the tiles' seam and across block lines. The
// footprints appear in the image (the replaced blocks' source triangles dropped, the patches drawn), sending the same D
// again rebuilds nothing, and clearing D gives back the image without it. Watertightness and exactness of the patch
// geometry are the CPU test's (unx_test_scene_terrainpatch); this checks the GPU path end to end. The same state drawn
// twice in a row must match (no pixel off by more than 0.1 in a channel); after D is cleared, R's world-space GI cache
// still holds entries lit with the dents (measured up to 0.12 in ~100-170 pixels), so that comparison requires the same
// geometry: no pixel off by more than 0.25 (a hole or a left-over dent differs by far more: the prints reach 0.58).
// Hardware GPU run under GpuLock -Kind correctness.
#include "Renderer/HostRenderer.h"

#include "unx/core/File.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 1280, kHeight = 720, kCells = 32;
constexpr float kCell = 0.5f;

float heightAt(float x, float z) { return 0.6f * std::sin(x * 0.21f) * std::cos(z * 0.17f) + 0.15f * std::sin(x * 0.9f + z * 0.5f); }

// A tile as UnravelNextTerrainCook writes it: terrain-space positions, (cells + 1)^2 vertices, diagonal a-d.
scene::Mesh tile(uint32_t x0)
{
    scene::Mesh m;
    m.name = "terrain tile";
    for (uint32_t j = 0; j <= kCells; ++j)
        for (uint32_t i = 0; i <= kCells; ++i)
        {
            const float x = (x0 + i) * kCell, z = j * kCell;
            m.positions.push_back({ x, heightAt(x, z), z });
            const float gx = (heightAt(x + kCell, z) - heightAt(x - kCell, z)) / (2 * kCell), gz = (heightAt(x, z + kCell) - heightAt(x, z - kCell)) / (2 * kCell);
            m.normals.push_back(normalize(float3{ -gx, 1, -gz }));
            m.uv0.push_back({ x / 32.0f, z / 16.0f });
        }
    for (uint32_t j = 0; j < kCells; ++j)
        for (uint32_t i = 0; i < kCells; ++i)
        {
            const uint32_t a = j * (kCells + 1) + i, b = a + 1, c = a + kCells + 1, d = c + 1;
            m.indices.insert(m.indices.end(), { a, c, d, a, d, b });
        }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    return m;
}

uint32_t g_worst = 0;  // largest channel difference of the last comparison (of 1023)

uint32_t differing(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
{
    uint32_t n = 0, worst = 0, x0 = kWidth, x1 = 0, y0 = kHeight, y1 = 0;
    for (size_t i = 0; i < a.size(); ++i)
        for (uint32_t c = 0; c < 3; ++c)
        {
            const uint32_t d = (uint32_t)std::abs((int)((a[i] >> (10 * c)) & 1023) - (int)((b[i] >> (10 * c)) & 1023));
            worst = std::max(worst, d);
            if (d > 102)
            {
                ++n;
                const uint32_t x = (uint32_t)(i % kWidth), y = (uint32_t)(i / kWidth);
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                break;
            }
        }
    if (n) logf("    [diff: %u px, worst channel %u/1023, box x %u..%u y %u..%u]\n", n, worst, x0, x1, y0, y1);
    g_worst = worst;
    return n;
}
} // namespace

int main()
{
    try
    {
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        HostRenderer h(o);
        scene::Scene& s = h.scene();
        s.name = "terrain patches";
        scene::Material grey;
        grey.name = "ground";
        grey.roughness = 0.8f;
        s.materials.push_back(grey);
        s.meshes = { tile(0), tile(kCells) };
        for (uint32_t k = 0; k < 2; ++k)
        {
            scene::Instance in;
            in.mesh = k;
            s.instances.push_back(in);
        }
        // Low sun from the side: dents cast and receive shadows.
        s.sun.direction = normalize(float3{ 0.6f, 0.35f, 0.3f });  // towards the sun
        scene::Camera camera;
        camera.name = "over the seam";
        camera.position = { 16.0f, 3.2f, 13.0f };
        camera.forward = normalize(float3{ 0, -0.55f, -1.0f });
        s.cameras.push_back(camera);
        // Room for 32 blocks at 5 cm (41^2 vertices, 3,200 triangles each).
        RuntimeCapacity capacity;
        capacity.meshes = 32, capacity.submeshes = 32, capacity.vertices = 32 * 1681, capacity.indices = 32 * 9600;
        capacity.clusters = 32 * 64, capacity.clusterVertexIndices = 32 * 64 * 128, capacity.clusterTriangles = 32 * 3200, capacity.nodes = 32 * 64;
        capacity.instances = 32;
        h.reserveRuntime(capacity);
        h.commit();

        uint64_t index = 0;
        std::vector<uint32_t> image(kWidth * kHeight);
        auto render = [&](uint32_t frames) {
            h.setDiscontinuity(kDiscontinuityCut);
            for (uint32_t f = 0; f < frames; ++f)
            {
                FramePacket p;
                p.frameIndex = index;
                p.time = index / 60.0;
                ++index;
                p.deltaTime = 1.0f / 60;
                p.width = kWidth;
                p.height = kHeight;
                p.camera = camera;
                const bool last = f + 1 == frames;
                h.renderStandalone(h.queueFrame(std::move(p)), last ? image.data() : nullptr, last ? image.size() * 4 : 0);
            }
            return image;
        };
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-72s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        auto same = [&](const char* what, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
            const uint32_t n = differing(a, b);
            logf("  (%s: %u pixels differ)\n", what, n);
            expect(what, n == 0);
        };
        // 64 frames: R's GI cache (200,000 entries, ~7,800 refreshed per frame) converges after a change; images compare converged states.
        const uint32_t settle = 64;
        const std::vector<uint32_t> baseline = render(settle);
        same("same scene twice: no pixel differs by more than 0.1", render(settle), baseline);

        // D window: 5 cm texels over x [10, 22.8), z [2, 14.8) m; ellipsoidal dents up to 8 cm, one across x = 16 (the
        // seam), others across block lines (every 2 m).
        const float spacing = 0.05f;
        const uint32_t texels = 256;
        const double ox = 10.0, oz = 2.0;
        std::vector<float> d((size_t)texels * texels, 0.0f);
        const float prints[][3] = { { 16.0f, 9.0f, 0.08f }, { 14.1f, 10.0f, 0.06f }, { 18.0f, 8.1f, 0.06f }, { 12.4f, 11.9f, 0.05f } };
        for (uint32_t tz = 0; tz < texels; ++tz)
            for (uint32_t tx = 0; tx < texels; ++tx)
            {
                const float x = (float)(ox + tx * spacing), z = (float)(oz + tz * spacing);
                float v = 0;
                for (const auto& p : prints)
                {
                    const float u = (x - p[0]) / 0.18f, w = (z - p[1]) / 0.32f, r2 = u * u + w * w;
                    if (r2 < 1) v = std::min(v, -p[2] * (1 - r2));
                }
                d[(size_t)tz * texels + tx] = v;
            }
        const uint32_t tiles[2] = { 0, 1 };
        h.setTerrainDeformation(ox, oz, spacing, texels, d.data(), tiles);
        const uint64_t built = h.patchBuildsForTest();
        const std::vector<uint32_t> dented = render(settle);
        const uint32_t changed = differing(dented, baseline);
        logf("  (%llu blocks built; %u pixels changed by the footprints)\n", (unsigned long long)built, changed);
        expect("footprints replace blocks in both tiles", built >= 6);
        expect("footprints are visible", changed > 500);
        same("drawn the same twice", render(settle), dented);

        h.setTerrainDeformation(ox, oz, spacing, texels, d.data(), tiles);
        expect("the same D again rebuilds nothing", h.patchBuildsForTest() == built);
        same("and draws the same", render(settle), dented);

        std::fill(d.begin(), d.end(), 0.0f);
        h.setTerrainDeformation(ox, oz, spacing, texels, d.data(), tiles);
        {
            const uint32_t n = differing(render(settle), baseline);
            logf("  (D cleared: %u pixels above 0.1, worst channel %u/1023)\n", n, g_worst);
            expect("D cleared: the terrain returns without it (worst <= 0.25)", g_worst <= 256);
        }

        logf(failures ? "HOST TERRAIN TEST FAILED (%u)\n" : "HOST TERRAIN TEST PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
