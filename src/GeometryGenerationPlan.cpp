#include "GeometryGenerationPlan.h"

#include <limits>
#include <stdexcept>
#include <unordered_set>

bool validEdgeSegments(uint32_t n) { return n == 1 || n == 2 || n == 4 || n == 8; }

GeometryGenerationPlan makeGeometryGenerationPlan(uint32_t sourceIndexCount, uint32_t sourceClusterCount,
    const ResolvedClusterSelection& selection, uint32_t edgeSegments, GeometryGenerationLimits limits)
{
    if (!validEdgeSegments(edgeSegments)) throw std::runtime_error("Edge segments must be 1, 2, 4 or 8.");
    if (selection.sceneRevision == 0) throw std::runtime_error("Compute input has no source revision.");
    uint64_t count = 0;
    std::unordered_set<uint32_t> ids;
    for (const auto& range : selection.ranges) {
        if (range.sourceClusterId >= sourceClusterCount || !ids.insert(range.sourceClusterId).second ||
            range.firstIndex % 3 != 0 || range.indexCount == 0 || range.indexCount % 3 != 0 ||
            uint64_t(range.firstIndex) + range.indexCount > sourceIndexCount) {
            throw std::runtime_error("Invalid compute input cluster range / ID.");
        }
        count += range.indexCount;
    }
    if (count != selection.indexCount || count > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Compute source count exceeds uint32 or disagrees with the selection.");
    }
    const uint64_t multiplier = uint64_t(edgeSegments) * edgeSegments;
    const uint64_t outputCount = count * multiplier; // count <= uint32, multiplier <= 64
    if (outputCount > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Subdivision output count exceeds uint32.");
    }
    GeometryGenerationPlan plan;
    plan.edgeSegments = edgeSegments;
    plan.sourceTriangleCount = static_cast<uint32_t>(count / 3);
    plan.vertexCount = plan.indexCount = static_cast<uint32_t>(outputCount);
    plan.vertexBytes = outputCount * 32; // explicit eight-word Vertex ABI
    plan.indexBytes = outputCount * 4;
    plan.worklistBytes = (count / 3) * 4;
    const uint64_t groups = (outputCount / 3 + kGeometryGroupSize - 1) / kGeometryGroupSize;
    if (plan.vertexBytes + plan.indexBytes > limits.maxOutputBytes ||
        plan.vertexBytes > limits.maxStorageBufferBytes || plan.indexBytes > limits.maxStorageBufferBytes ||
        plan.worklistBytes > limits.maxStorageBufferBytes || groups > limits.maxWorkgroupsX) {
        throw std::runtime_error("Geometry generation exceeds the output budget, storage-buffer range, or dispatch limit.");
    }
    plan.workgroupsX = static_cast<uint32_t>(groups);
    // All size checks precede allocation/expansion of the potentially large list.
    plan.triangleFirstIndices.reserve(static_cast<size_t>(count / 3));
    plan.ranges.reserve(selection.ranges.size());
    uint32_t outputFirst = 0;
    for (const auto& range : selection.ranges) {
        const auto outputRangeCount = static_cast<uint32_t>(range.indexCount * multiplier);
        plan.ranges.push_back({range.sourceClusterId, outputFirst, outputRangeCount});
        for (uint32_t i = 0; i < range.indexCount; i += 3) plan.triangleFirstIndices.push_back(range.firstIndex + i);
        outputFirst += outputRangeCount;
    }
    return plan;
}

bool sameClusterSelection(const ResolvedClusterSelection& a, const ResolvedClusterSelection& b)
{
    if (a.sceneRevision != b.sceneRevision || a.allClusters != b.allClusters ||
        a.indexCount != b.indexCount || a.ranges.size() != b.ranges.size()) return false;
    for (size_t i = 0; i < a.ranges.size(); ++i) {
        if (a.ranges[i].sourceClusterId != b.ranges[i].sourceClusterId ||
            a.ranges[i].firstIndex != b.ranges[i].firstIndex || a.ranges[i].indexCount != b.ranges[i].indexCount) return false;
    }
    return true;
}
