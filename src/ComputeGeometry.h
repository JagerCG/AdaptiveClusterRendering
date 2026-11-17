#pragma once

#include "GeometryGenerationPlan.h"
#include "SceneGpuData.h"
#include "HeightField.h"

#include <chrono>

// Borrowed output handles. ComputeGeometry owns the buffers; replacement, empty
// generation or destruction invalidates old handles. Check valid + source revision.
struct GeneratedGeometry {
    bool valid = false;
    SceneRevision sourceRevision = 0;
    uint64_t generationRevision = 0;
    VkBuffer vertexBuffer = VK_NULL_HANDLE, indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0, indexOffset = 0;
    uint32_t vertexStride = 32, indexStride = 4;
    uint32_t vertexCount = 0, indexCount = 0;
    uint32_t edgeSegments = 1, sourceTriangleCount = 0;
    std::vector<GeneratedDrawRange> ranges;
    uint64_t vertexBytes = 0, indexBytes = 0, allocatedOutputBytes = 0;
    uint64_t worklistBytes = 0, allocatedWorklistBytes = 0;
    uint32_t workgroups = 0;
    float maxDisplacement = 0.0f; // conservative object-space padding for source AABBs
    uint64_t heightRevision = 0;
    DisplacementSettings displacement;
    double cpuGenerateMs = 0.0;
    std::optional<double> gpuGenerateMs;
};

class ComputeGeometry {
public:
    ~ComputeGeometry();
    ComputeGeometry() = default;
    ComputeGeometry(const ComputeGeometry&) = delete;
    ComputeGeometry& operator=(const ComputeGeometry&) = delete;

    void initialize(VkPhysicalDevice gpu, VkDevice device, uint32_t queueFamily);
    // Caller waits for frame completion before prepare/destroy, and owns command
    // reset/begin/end/submission. finish is called only after submission completes.
    void prepare(const SceneGpuData& source, const ResolvedClusterSelection& selection, uint32_t edgeSegments,
                 const DisplacementSettings& displacement);
    void record(VkCommandBuffer command);
    void finish();
    void invalidate() { output_.valid = false; }
    bool matches(const SceneGpuData& source, const ResolvedClusterSelection& selection, uint32_t edgeSegments,
                 const DisplacementSettings& displacement) const;
    const GeneratedGeometry& output() const { return output_; }
    // Caller has waited for the frame fence. Uses the supplied resettable graphics/
    // compute command buffer synchronously; publication occurs only after upload.
    void uploadHeight(const HeightField& field, VkCommandBuffer command, VkQueue queue);
    const HeightField& heightField() const { return heightField_; }
    uint64_t heightRevision() const { return heightRevision_; }
    uint64_t heightAllocationBytes() const { return height_.allocationBytes; }

private:
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize allocationBytes = 0;
    };
    void createBuffer(Buffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    void destroyBuffer(Buffer& buffer);
    struct HeightImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDeviceSize allocationBytes = 0;
    };
    void destroyHeight(HeightImage& height);
    HeightImage height_;
    HeightField heightField_;
    uint64_t heightRevision_ = 0;

    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    uint32_t timestampBits_ = 0;
    float timestampPeriod_ = 0;
    GeometryGenerationLimits limits_;
    Buffer vertices_, indices_, work_;
    SceneGpuData source_;
    ResolvedClusterSelection selection_;
    GeneratedGeometry output_;
    uint64_t generationCounter_ = 0;
    bool pending_ = false;
    std::chrono::steady_clock::time_point started_;
};
