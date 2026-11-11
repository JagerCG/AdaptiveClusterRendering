#pragma once

#include "ClusterGpu.h"
#include "MeshSource.h"
#include <vulkan/vulkan.h>

static_assert(sizeof(Vertex) == 32 && offsetof(Vertex, normal) == 12 && offsetof(Vertex, texcoord) == 24);

// Non-owning view. VulkanRenderer owns these fully resident resources. Handles
// remain valid until the next successful uploadScene() or renderer destruction.
// Vertex and index views may share an allocation; offsets must be respected.
struct SceneGpuData {
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
