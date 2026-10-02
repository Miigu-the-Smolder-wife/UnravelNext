// V internal: layout of a raster request's software raster buffers (visibility.software_raster; DepthRasterSw.hlsl,
// DepthSwMerge.ms / .ps). Owner: V. C++ mirror: VisibilityTrack.cpp kDsa*, kDsp*.
#ifndef UNX_DEPTH_RASTER_SW_HLSLI
#define UNX_DEPTH_RASTER_SW_HLSLI

// Software arguments (raw, 8 words).
#define DSA_CLEAR 0u   // 0..2 DispatchIndirect: one group per page given out
#define DSA_MERGE 3u   // 3..5 DispatchMesh: one group per DSW_MERGE_PAGES pages given out
#define DSA_WORDS 8u   // (6, 7: unused)

// Page slots (raw).
#define DSP_TILES 0u   // set tiles of the request's views (may exceed the page capacity: the need)
#define DSP_FIRST 1u   // + page: the atlas slot of the page's tile

#define DSW_MERGE_PAGES 32u  // pages (quads) per mesh group of the merge

#endif
