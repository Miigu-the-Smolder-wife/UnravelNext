// Local exposure (shading.post_local_exposure; Post.cpp): the image's contrast between its large bright and dark regions
// is scaled down while the detail inside them is kept - a window in a dim room stays readable and the room does not go
// black. The structure and the default numbers follow Unreal Engine's local exposure, bilateral method (ue6-main
// PostProcessLocalExposure.usf, PostProcessHistogramCommon.ush CalculateBaseLogLuminance / CalculateLogLocalExposure,
// PostProcessHistogram.usf BILATERAL_GRID, read 2026-10-02); the code is ours.
//   grid     a bilateral grid of the exposed image's log2 luminance: tiles of 128 x 128 output pixels (the reference: 64 x
//            64 texels of its half-resolution image) by LE_DEPTH luminance buckets; a cell holds (sum of log luminance x
//            weight, sum of weight), every sample split between its two nearest buckets (LocalExposureGrid.hlsl);
//   blurred  the tiles' mean log luminance under a Gaussian of radius a quarter of the view's width, the reference's
//            kernel of 50 % (LocalExposureBlur.hlsl);
//   base     at a pixel: the grid at (uv, the pixel's own luminance), trilinear - the mean of what is as bright as the pixel
//            around it - blended with the blurred value (0.6 by default);
//   scale    log local = middle grey + (base - middle grey) x contrast (highlights and shadows apart) + (log lum - base) x
//            detail strength; the pixel is multiplied by 2^(log local - log lum).
// Luminance here is exposed (the image as the chain gets it): middle grey is 0.18.
#ifndef UNX_LOCAL_EXPOSURE_HLSLI
#define UNX_LOCAL_EXPOSURE_HLSLI
#include "Bindless.hlsli"

#define LE_DEPTH 32u          // luminance buckets (BILATERAL_GRID_DEPTH)
#define LE_TILE 128u          // output pixels per grid cell
#define LE_LOG_MIN -14.0      // log2 of the exposed luminance of the first bucket ...
#define LE_LOG_MAX 10.0       // ... and of the last
#define LE_LUM_MIN 6.1e-5     // (2^-14: darker values count as this)

float leLuminance(float3 exposed) { return max(dot(max(exposed, 0.0), float3(0.2126, 0.7152, 0.0722)), LE_LUM_MIN); }
float leBucketPosition(float logLum) { return saturate((logLum - LE_LOG_MIN) / (LE_LOG_MAX - LE_LOG_MIN)); }

struct LeParams
{
    uint grid, blurred;       // SRVs: Texture3D<float2>, Texture2D<float>
    float2 uvScale;           // view size / (LE_TILE x the grid's size): the last row and column overhang the view
    float highlight, shadow;  // contrast scales of regions above / below middle grey (1 = off)
    float detail;             // detail strength (1 = unchanged)
    float blend;              // share of the blurred luminance in the base
    float logMiddleGrey;      // log2(0.18 x 2^bias)
};

// The factor of a pixel of exposed colour 'exposed' at view uv ([0, 1]^2). logScale: log2 of a factor the caller applies
// to the whole image besides (a snap frame's exposure correction): the regions are placed against middle grey with it.
float leScale(LeParams p, float3 exposed, float2 uv, float logScale)
{
    const float logLum = log2(leLuminance(exposed));
    Texture3D<float2> grid = ResourceDescriptorHeap[p.grid];
    Texture2D<float> blurred = ResourceDescriptorHeap[p.blurred];
    const float2 guv = uv * p.uvScale;
    const float2 cell = grid.SampleLevel(g_linearClamp, float3(guv, (leBucketPosition(logLum) * (LE_DEPTH - 1) + 0.5) / LE_DEPTH), 0);
    const float blurredLum = blurred.SampleLevel(g_linearClamp, guv, 0);
    // (a cell without samples - the grid is built from every second pixel - falls back to the blurred value)
    const float bilateral = cell.y < 0.001 ? blurredLum : cell.x / cell.y;
    const float base = lerp(bilateral, blurredLum, p.blend) + logScale;
    const float centred = base - p.logMiddleGrey;
    const float local = p.logMiddleGrey + centred * (centred > 0 ? p.highlight : p.shadow) + (logLum + logScale - base) * p.detail;
    return exp2(local - (logLum + logScale));
}
#endif
