#pragma once
#include "ClusterSelection.h"
#include "SceneData.h"
#include "HeightField.h"

struct GeometryValidationStats {
    uint64_t triangles = 0, sharedEdgeChecks = 0;
    double maxAttributeError = 0;
    double maxNormalError = 0;
};

// Debug/reference path only. Never used to generate the displayed geometry.
GeometryValidationStats validateGeneratedGeometry(const SceneData& source,
    const ResolvedClusterSelection& selection, uint32_t edgeSegments,
    const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
    const HeightField* height = nullptr, DisplacementSettings displacement = {});
