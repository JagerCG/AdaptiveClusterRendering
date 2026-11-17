#pragma once

#include "MeshSource.h"
#include <unordered_set>

// Contract edges onto existing endpoints so retained vertices keep their UVs
// and normals. Replaces patch.indices and returns a model-space error bound.
double simplifyPatch(MeshSource& patch, const std::unordered_set<uint32_t>& lockedPositions);
