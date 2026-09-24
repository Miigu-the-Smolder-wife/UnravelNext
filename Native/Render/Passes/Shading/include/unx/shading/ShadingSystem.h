#pragma once
// M track, shading (ARCHITECTURE 2.11; INTERFACES 5.2, 7.5): per shade class one kernel over that class's tiles
// (material resolve's lists, ExecuteIndirect), writing the view's final colour.
#include "unx/render/Frame.h"

#include <vector>

namespace unx::render::shading
{
// Specular directional albedo split by f0 (ShadingCommon.hlsli shSpecularAB): 32 x 32 (A, B) on the model's E grid,
// same visible-normal samples as scene::model::directionalAlbedoTable, so A + B = E to float rounding.
const std::vector<float>& specularAlbedoTable();

void shade(FramePassContext& fc, ViewResources& view);
} // namespace unx::render::shading
