// Blue noise for stochastic passes (FrameConstants::blueNoise; unx/render/BlueNoise.h): a 64 x 64 tile of four independent
// void-and-cluster patterns. blueNoise4(pixel, index) = the tile at the pixel, each channel rotated by a golden-ratio
// multiple of the index (a frame or sample counter): every index's pattern is blue in space - a spatial filter or the
// eye sees its error as fine grain, not blotches - and a pixel's values over successive indices are a low-discrepancy
// sequence, so a temporal filter converges as 1 / n. Unreal's stochastic passes read the same kind of table
// (BlueNoise.ush). Two uses in one kernel take different channels or a shifted pixel.
#ifndef UNX_BLUE_NOISE_HLSLI
#define UNX_BLUE_NOISE_HLSLI
#include "Frame.hlsli"

uint blueNoiseHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float4 blueNoise4(uint2 pixel, uint index)
{
    float4 v;
    if (g_blueNoise != UNX_NONE)
    {
        Texture2D<float4> tile = ResourceDescriptorHeap[g_blueNoise];
        v = tile.Load(int3(pixel & 63u, 0));
    }
    else
    {
        // (no tile: white noise per pixel)
        const uint s = blueNoiseHash(pixel.x * 0x9E3779B1u + pixel.y * 0x85EBCA77u + 0xC2B2AE3Du);
        v = float4(blueNoiseHash(s) >> 8, blueNoiseHash(s + 1) >> 8, blueNoiseHash(s + 2) >> 8, blueNoiseHash(s + 3) >> 8) * (1.0 / 16777216.0);
    }
    // (the index below 1024: its products keep 13 fractional bits in a float)
    return frac(v + float(index & 1023u) * float4(0.61803398875, 0.75487766625, 0.56984029099, 0.38196601125));
}
float2 blueNoise2(uint2 pixel, uint index) { return blueNoise4(pixel, index).xy; }
float blueNoise1(uint2 pixel, uint index) { return blueNoise4(pixel, index).x; }

#endif
