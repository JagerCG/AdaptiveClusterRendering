#pragma once

#include "MeshSource.h"
#include <unordered_set>

// Contract edges onto existing endpoints so retained vertices keep their UVs
// and normals. maxPlaneDistance limits each removed corner's distance to the
// replacement face planes. Returns a conservative bound on vertex travel.
double simplifyPatch(MeshSource& patch, const std::unordered_set<uint32_t>& lockedPositions, double maxPlaneDistance);

// Merge vertices in a small spatial grid for a complete distant region. Returns a conservative error bound.
double simplifyDistantPatch(MeshSource& patch, unsigned cellsPerAxis);
