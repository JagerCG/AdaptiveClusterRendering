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
    uint64_t residentAllocationBytes = 0; // mesh + clusters, incl. alignment; excludes staging
    double cpuUploadMs = 0.0; // validation, packing, allocation, staging, submission + wait
    std::optional<double> gpuTransferMs; // GPU timestamps around the two buffer copies
};

// Non-owning view. VulkanRenderer owns these fully resident resources. Handles
// remain valid until the next successful uploadScene() or renderer destruction.
// Vertex and index views may share an allocation; offsets must be respected.
struct SceneGpuData {
    SceneRevision sceneRevision = 0; // increments on every successful source upload
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
};
