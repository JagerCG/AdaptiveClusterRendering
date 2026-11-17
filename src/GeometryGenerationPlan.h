#pragma once

#include "ClusterSelection.h"

constexpr uint32_t kGeometryGroupSize = 64;
constexpr uint64_t kDefaultGeometryOutputBudget = 128ull * 1024 * 1024;

struct GeneratedDrawRange {
    uint32_t sourceClusterId;
    uint32_t firstIndex;
    uint32_t indexCount;
};

struct GeometryGenerationLimits {
    uint64_t maxOutputBytes = kDefaultGeometryOutputBudget;
    uint64_t maxStorageBufferBytes = 128ull * 1024 * 1024;
    uint32_t maxWorkgroupsX = 65535;
};

struct GeometryGenerationPlan {
    // CPU expands ranges into source triangle addresses only; vertices/indices
    // are read and emitted by the compute shader, never generated here.
    std::vector<uint32_t> triangleFirstIndices;
    std::vector<GeneratedDrawRange> ranges;
    uint32_t edgeSegments = 1, sourceTriangleCount = 0;
    uint32_t vertexCount = 0; // three unshared vertices per generated triangle
    uint32_t indexCount = 0;
    uint32_t workgroupsX = 0;
    uint64_t vertexBytes = 0, indexBytes = 0, worklistBytes = 0;
};

bool validEdgeSegments(uint32_t n);
GeometryGenerationPlan makeGeometryGenerationPlan(uint32_t sourceIndexCount, uint32_t sourceClusterCount,
    const ResolvedClusterSelection& selection, uint32_t edgeSegments = 1, GeometryGenerationLimits limits = {},
    bool expandCpuWork = true);
bool sameClusterSelection(const ResolvedClusterSelection& a, const ResolvedClusterSelection& b);
