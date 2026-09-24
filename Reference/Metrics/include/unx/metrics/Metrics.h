#pragma once
// Image metrics (INTERFACES_KO.md 10.3; ARCHITECTURE 3). Skeleton by core, owned by C from here on.
//   Images: linear RGB float, rows top to bottom. File exchange format: PFM ("PF", little endian, bottom-to-top rows
//   on disk as the format requires; read/write convert).
//   relMSE      mean over pixels and channels of (t - r)^2 / (r^2 + 0.01)
//   HDR-FLIP    NVIDIA FLIP v1.7 HDR mode on linear radiance x exposure; mean and 99th percentile of the error map
//   LDR-FLIP    FLIP LDR mode on display values in [0,1] (after the renderer's tonemap, linear before OETF)
//   temporal    for a static camera: mean over pixels of |Y_t - Y_{t-1}| / (mean Y + 1e-6), Y = Rec.709 luminance
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace unx::metrics
{
struct Image
{
    uint32_t width = 0, height = 0;
    std::vector<float> rgb;  // 3 floats per pixel, row-major, top row first
    float* pixel(uint32_t x, uint32_t y) { return &rgb[3 * ((size_t)y * width + x)]; }
    const float* pixel(uint32_t x, uint32_t y) const { return &rgb[3 * ((size_t)y * width + x)]; }
};

Image readPfm(const std::filesystem::path& path);
void writePfm(const std::filesystem::path& path, const Image& image);

double relMse(const Image& reference, const Image& test);

struct FlipResult
{
    double mean = 0;
    double p99 = 0;
    std::vector<float> errorMap;  // one value per pixel
};
FlipResult flipHdr(const Image& reference, const Image& test);
FlipResult flipLdr(const Image& reference, const Image& test);

// Per consecutive pair of frames; returns one value per pair.
std::vector<double> temporalInstability(const std::vector<Image>& frames);
} // namespace unx::metrics
