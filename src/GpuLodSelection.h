#pragma once

#include "LodBuilder.h"
#include "OrbitCamera.h"

#include <vulkan/vulkan.h>
#include <glm/vec4.hpp>

#include <array>

enum class LodMode {
    Manual,
    Distance,
    GpuLocal
};

enum class MeshColorMode {
    Shaded,
    Cluster,
    Group,
    Lod
};

struct LodSettings {
    LodMode mode = LodMode::Manual;
    uint32_t level = 0;
    float pixelError = 8.0f;
};

struct LodStats {
    uint32_t clusters = 0;
    uint32_t triangles = 0;
    std::array<uint32_t, 8> levels{};
};

struct alignas(16) GpuLodNode {
    glm::vec4 minimumError;
    glm::vec4 maximum;
    glm::uvec4 links;
};

struct alignas(16) GpuCluster {
    glm::uvec4 geometry;
    glm::uvec4 colors;
};

struct PackedLodScene {
    MeshSource mesh;
    std::vector<GpuLodNode> nodes;
    std::vector<GpuCluster> clusters;
    glm::vec4 centerRadius;
    uint32_t levelCount = 0;
};

PackedLodScene packLodScene(const LodHierarchy& hierarchy);

class GpuLodSelection {
public:
    GpuLodSelection() = default;
    ~GpuLodSelection();
    GpuLodSelection(const GpuLodSelection&) = delete;
    GpuLodSelection& operator=(const GpuLodSelection&) = delete;

    void initialize(VkPhysicalDevice gpu, VkDevice device);
    void upload(const PackedLodScene& scene);
    void record(VkCommandBuffer commands, const CameraView& camera, const LodSettings& settings);
    void readStats();
    VkDescriptorSetLayout descriptorLayout() const { return descriptorLayout_; }
    VkDescriptorSet descriptorSet() const { return descriptorSet_; }
    VkBuffer indirectBuffer() const { return buffers_[2].handle; }
    uint32_t commandCount() const { return clusterCount_; }
    const LodStats& stats() const { return stats_; }

private:
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void destroyBuffer(Buffer& buffer);

    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::array<Buffer, 4> buffers_{};
    glm::vec4 centerRadius_{};
    uint32_t clusterCount_ = 0;
    uint32_t levelCount_ = 0;
    LodStats stats_;
};
