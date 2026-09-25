// Specular directional albedo of material model v1 split by f0 (INTERFACES 8.1): 32 x 32 (A, B) pairs on the model's
// E grid (row = roughness, column = NoV), E_ss(f0) = f0 A + B, A + B = E. Built with the same visible-normal samples
// as scene::model::directionalAlbedoTable and M's ShadingSystem table (read by ShadingCommon.hlsli shSpecularAB), so the
// ray hits' shading (HitShading.hlsli) equals the direct view's. R's copy until the table moves into scene::model with a
// frame constant SRV (request Docs/Design/Requests/20260925_R_hit_shading.md).
#pragma once
#include "unx/render/Device.h"

#include <cstdint>
#include <vector>

namespace unx::render::rt
{
const std::vector<float>& specularAlbedoTable();

// GPU copy (StructuredBuffer<float2>, 32 x 32) shared by R's systems on a device; bindless SRV index.
uint32_t specularAlbedoSrv(Device& device);
void releaseSpecularAlbedo(Device& device);
} // namespace unx::render::rt
