#pragma once
#include <cstdint>

namespace unx::render::shading::detail
{
// Same optional-input classes as the rejection A/B fixture: P[2].y/z/w,
// then the resurrection measure + guide pair in P[3].x/y.
enum TsrRejectOptional : uint32_t
{
    TsrRejectMoire = 1u,
    TsrRejectThin = 2u,
    TsrRejectLayers = 4u,
    TsrRejectResurrection = 8u,
    TsrRejectAll = 15u
};
constexpr bool useFusedTsrRejection(uint32_t width, uint32_t height, uint32_t validOptionalInputs)
{
    // The production moire/thin/layer combination also benefits without
    // resurrection: 1280x720 paired GPU result 0.4414 -> 0.3154 ms.
    // Both paths preserve the same filter halo and quantization.
    constexpr uint32_t filterInputs = TsrRejectMoire | TsrRejectThin | TsrRejectLayers;
    return (width >= 1920 && height >= 1080) ||
           (width >= 1280 && height >= 720 && (validOptionalInputs & filterInputs) == filterInputs);
}
} // namespace unx::render::shading::detail
