#pragma once

#include "ClusterGpu.h"
#include "ClusterSelection.h"
#include "MeshSource.h"
#include "SceneDiagnostics.h"
#include <vulkan/vulkan.h>

static_assert(sizeof(Vertex) == 32 && offsetof(Vertex, normal) == 12 && offsetof(Vertex, texcoord) == 24);

// Last successful upload; excludes verifyUpload() readback and rendering.
struct SceneUploadStats {
    ScenePayloadBytes payload;
    uint64_t residentAllocationBytes = 0; // all geometry and hierarchy, incl. alignment; excludes staging
    double cpuUploadMs = 0.0; // validation, packing, allocation, staging, submission + wait
    std::optional<double> gpuTransferMs; // GPU timestamps around the two buffer copies
};

// All levels share the vertex array. Bind the cluster storage buffer at zero;
// the three metadata offsets below are in 32-bit words. LodLevel / LodNode are
// std430 records. Traversal uses absolute node/cluster IDs and index ranges.
struct ResidentHierarchy {
    VkDeviceSize allIndexOffset = 0;
    uint32_t totalIndices = 0, totalClusters = 0;
    uint32_t levelWord = 0, nodeWord = 0, childWord = 0;
    uint32_t levelCount = 0, nodeCount = 0, childCount = 0;
};

// Non-owning view. VulkanRenderer owns these fully resident resources. Handles
// remain valid until the next successful uploadScene() or renderer destruction.
// Vertex and index views may share an allocation; offsets must be respected.
struct SceneGpuData {
    SceneRevision sceneRevision = 0; // active partition identity, including manual level changes
    ResidentHierarchy hierarchy;
    uint32_t activeLevel = 0, firstCluster = 0; // local draw IDs + firstCluster = absolute IDs
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkBuffer clusterBuffer = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0;
    VkDeviceSize indexOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t clusterCount = 0;
    uint32_t vertexStride = sizeof(Vertex);
    uint32_t indexStride = sizeof(uint32_t);
    uint32_t clusterStride = sizeof(GpuClusterDesc);
    VkDescriptorSet clusterSet = VK_NULL_HANDLE; // set 0, binding 0, readonly storage buffer
    // Borrowed from the GPU selector for mixed cuts. Word 4 starts four
    // VkDispatchIndirectCommands (n=1,2,4,8); work contains absolute index addresses.
    static constexpr uint32_t lodRecordStride = sizeof(uint32_t) + sizeof(VkDrawIndexedIndirectCommand);
    VkBuffer lodCommands = VK_NULL_HANDLE, lodTriangleWork = VK_NULL_HANDLE;
    VkDeviceSize lodRecordOffset = 0, lodWorkAllocation = 0;
};
