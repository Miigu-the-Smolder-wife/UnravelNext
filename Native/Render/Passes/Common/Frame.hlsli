// Frame constants (root CBV b1), mirror of unx::render::gpu::FrameConstants (GpuSceneLayout.h). One per view.
#ifndef UNX_FRAME_HLSLI
#define UNX_FRAME_HLSLI

#define UNX_NONE 0xFFFFFFFFu
#define VIEW_MAIN 0u
#define VIEW_PLANAR_REFLECTION 1u

cbuffer FrameConstants : register(b1)
{
    row_major float4x4 g_viewProj;
    row_major float4x4 g_prevViewProj;
    row_major float4x4 g_invViewProj;
    row_major float4x4 g_view;
    row_major float4x4 g_proj;
    float3 g_cameraPosition;
    float g_nearPlane;
    float4 g_clipPlane;
    uint g_viewWidth, g_viewHeight, g_viewKind, g_frameIndex;
    float g_time, g_deltaTime, g_exposure, g_tanHalfFovY;
    float3 g_sunDirection;
    float g_sunIlluminance;
    float3 g_sunColor;
    float g_sunAngularRadius;
    float3 g_windDirection;
    float g_windSpeed;
    uint g_instances, g_meshes, g_submeshes, g_vertices;
    uint g_indices, g_clusters, g_clusterVertexIndices, g_clusterTriangles;
    uint g_lodLevels, g_materials, g_materialRemap, g_lights;
    uint g_skinVertices, g_bonePalette, g_prevBonePalette, g_materialModelLut;
    uint g_instanceCount, g_meshCount, g_clusterCount, g_lightCount;
    uint g_materialCount, g_sceneRevision, g_lodLevelClusters, g_specularAlbedoLut;
    uint g_coverageMaskLut, g_giRaysThisFrame, g_debugDraw;
    float g_viewModelScale;  // view-model projection remap, 1 = none (ViewModel.hlsli)  // g_debugDraw: DebugDraw.hlsli (0xFFFFFFFF = off)
    uint g_morphRecords, g_morphData, g_patchData;  // C4 morphs, C5 terrain patch slots (VisibilityCommon.hlsli)
    uint g_terrainLayers;                           // v1.74 Terrain-class material layers (Scene.hlsli GpuTerrainLayer)
    uint g_materialLayers, g_coatTable;  // v1.76 A9 layer records, coat tables (MaterialModel.hlsli)
    uint g_fxLightCount, g_fxLightCapacity;  // v1.79 A3 FX lights: StructuredBuffer<uint> [0] = F at lights[g_lightCount..) (kNone: none), F_max
    float g_upscaleRatio;  // internal / output height of an upscaled main view, 0 = native (GpuSceneLayout.h)
    uint g_blueNoise;  // the blue-noise tile's SRV (BlueNoise.hlsli), UNX_NONE: none
    uint g_fog;  // the view's fog parameters' SRV + 1 (FogVolume.hlsli), 0: none
    uint g_materialInputs;  // StructuredBuffer<GpuMaterialInputs> (Scene.hlsli), UNX_NONE: none
    uint g_meshAttributes, g_vertexAttributes;  // per mesh 1 + its first GpuVertexAttributes (0: none); UNX_NONE: no mesh has any
    uint g_clusterStream;  // compressed cluster vertices (ClusterStream.hlsli): the raw words' SRV + 1, 0: none
    // (the C++ record ends with one spare word; it is not declared here: a library kernel at the DXIL limit pays for
    // every field's annotation)
};

// Reversed-Z infinite projection: device depth d = near / viewDistance (1 at the near plane, 0 = sky).
float linearDepth(float deviceDepth) { return g_nearPlane / max(deviceDepth, 1e-30); }

// World position of a pixel centre (pixel in [0, size)) at a device depth.
float3 worldFromDepth(float2 pixel, float deviceDepth)
{
    const float2 ndc = float2((pixel.x + 0.5) / g_viewWidth * 2 - 1, 1 - (pixel.y + 0.5) / g_viewHeight * 2);
    const float4 p = mul(g_invViewProj, float4(ndc, deviceDepth, 1));
    return p.xyz / p.w;
}

bool clipPlaneKeeps(float3 world) { return all(g_clipPlane == 0) || dot(g_clipPlane.xyz, world) + g_clipPlane.w >= 0; }

#endif
