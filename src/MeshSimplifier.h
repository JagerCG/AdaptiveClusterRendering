#pragma once

#include "MeshSource.h"

struct SimplificationResult {
    MeshSource mesh;
    float vertexError = 0.0f;
};

SimplificationResult simplifyMesh(const MeshSource& mesh, float triangleRatio,
                                   const std::vector<uint32_t>& lockedPositions = {});
