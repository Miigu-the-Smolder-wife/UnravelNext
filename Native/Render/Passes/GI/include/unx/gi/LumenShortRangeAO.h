#pragma once
// Short-range AO and bent normal for the screen-probe final gather (lumen.short_range_ao; LumenShortRangeAO.hlsli states
// the passes; owner A, for R's gi.lumen). Follows the structure and defaults of Unreal's Lumen short-range AO.
#include "unx/render/Frame.h"

namespace unx::render::gi
{
// Records r.gi.sao and r.gi.sao.temporal for the main view and returns the full-resolution result (RGBA16F: xyz = world
// bent normal x AO, a = accumulated frames + 1, 0 = no surface); invalid when the switch is off or the view lacks depth
// or G-buffer. The caller stores it in ViewResources::shortRangeAO.
TextureRef lumenShortRangeAO(FramePassContext& fc, const ViewResources& view);
} // namespace unx::render::gi
