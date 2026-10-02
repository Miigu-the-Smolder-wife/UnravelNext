#pragma once
// Blue noise for the stochastic passes (FrameConstants::blueNoise, Passes/Common/BlueNoise.hlsli): a 64 x 64 tile of four
// independent void-and-cluster patterns (Ulichney 1993), each a permutation of the ranks 0..4095 stored as unorm16.
// A pass adds a golden-ratio multiple of its frame index to the tile's value (a Cranley-Patterson rotation): every
// frame's pattern is then blue in space, and a pixel's values over frames are a low-discrepancy sequence.
// Unreal reads the same kind of table (BlueNoise.ush) for its screen-probe rays, reflection rays and dithers.
#include "unx/render/Device.h"

#include <cstdint>
#include <vector>

namespace unx::render
{
constexpr uint32_t kBlueNoiseSize = 64;
// The tile's texels: size x size x 4 unorm16 (channel c = pattern c). Made once per process (about 0.1 s).
const std::vector<uint16_t>& blueNoiseTile();
// A texture of the tile on 'device' (R16G16B16A16_UNORM) with its bindless SRV. Blocking (creation time only).
struct BlueNoiseTexture
{
    ComPtr<ID3D12Resource> texture;
    uint32_t srv = 0xFFFFFFFFu;
};
BlueNoiseTexture createBlueNoise(Device& device);
} // namespace unx::render
