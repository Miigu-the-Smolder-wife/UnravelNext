#pragma once
// Small procedural meshes for RppBuild content (lodge door walls, game bench arena and bodies): flat-shaded boxes and
// capped cylinders with normals, unit tangents (w = +1) and planar UVs, counter-clockwise front faces (INTERFACES 6.1).
#include "unx/scene/SceneData.h"

#include <string>

namespace unx::rpp
{
// Axis-aligned box [lo, hi]; UV = 0.5 per metre (C's solid()).
scene::Mesh boxMesh(const std::string& name, float3 lo, float3 hi, uint32_t material);
// Cylinder along +Y from y = -halfHeight to +halfHeight, 'sides' facets, caps included; UV around = arc length.
scene::Mesh cylinderMesh(const std::string& name, float radius, float halfHeight, uint32_t sides, uint32_t material);
} // namespace unx::rpp
