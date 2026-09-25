// R diagnostic (CPU only, no GPU): the LOD cuts V's cluster builder makes for every skinned mesh of a scene, the input of
// error-bounded character RT proxies (a cut's object-space error against the ray footprint at the character's distance).
// Prints per cut its triangles and error, and the distance beyond which the error is below one pixel's footprint at 4K
// and 1440p (60 deg vertical field of view): d >= error x scale / pixel angle.
//
//   unx_gate_gi_proxylevels --scene <file.unxscene>
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/GpuScene.h"

#include <cmath>
#if UNX_HAS_CLUSTERBUILDER
#include "unx/clusterbuilder/ClusterBuilder.h"
#endif

using namespace unx;
using namespace unx::render;

int main(int argc, char** argv)
{
    try
    {
        std::string scenePath;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--scene" && i + 1 < argc) scenePath = argv[++i];
            else fail("unknown argument %s", a.c_str());
        }
        if (scenePath.empty()) fail("--scene <file.unxscene> is required");
#if UNX_HAS_CLUSTERBUILDER
        const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        const scene::Scene s = scene::load(scenePath);
        const ClusterData cd = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality));
        const double tanHalf = std::tan(60.0 * 3.14159265358979 / 360.0);
        const double pixel4K = 2 * tanHalf / 2160, pixel1440 = 2 * tanHalf / 1440;
        std::vector<bool> skinned(s.meshes.size(), false);
        std::vector<uint32_t> users(s.meshes.size(), 0);
        for (const scene::Instance& in : s.instances)
            if (in.mesh < s.meshes.size())
            {
                ++users[in.mesh];
                if (in.flags & scene::InstanceSkinned) skinned[in.mesh] = true;
            }
        for (uint32_t m = 0; m < (uint32_t)s.meshes.size(); ++m)
        {
            if (!skinned[m] || m >= cd.meshes.size()) continue;
            const auto& range = cd.meshes[m];
            logf("mesh %u '%s': %zu source triangles, %u skinned instances, %u cuts\n", m, s.meshes[m].name.c_str(), s.meshes[m].indices.size() / 3, users[m],
                 range.lodLevelCount);
            for (uint32_t l = range.lodLevelOffset; l < range.lodLevelOffset + range.lodLevelCount; ++l)
            {
                const gpu::LodLevel& level = cd.lodLevels[l];
                logf("  cut %2u: %7u triangles, error %.5f m -> below a 4K pixel beyond %.2f m, a 1440p pixel beyond %.2f m\n", l - range.lodLevelOffset,
                     level.triangleCount, level.error, level.error / pixel4K, level.error / pixel1440);
            }
        }
        return 0;
#else
        fail("needs the integrated build (Tools/CI/Build.ps1 -Track all): V's cluster builder is not in this one");
#endif
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
