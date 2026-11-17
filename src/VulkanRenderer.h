#pragma once

#include <vulkan/vulkan.h>
#include "SceneGpuData.h"
#include "ComputeGeometry.h"

#include <memory>
#include <vector>

struct GLFWwindow;
struct ClusterDataset;
struct ImDrawData;
struct MeshSource;
struct SceneData;
class OrbitCamera;

struct SourceDrawStats {
    bool usesGeneratedGeometry = false;
    bool wireframe = false;
    uint64_t triangles = 0;
    uint32_t meshDrawCalls = 0;
    uint32_t boundsInstances = 0;
    float boundsPadding = 0;
};

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void initialize(GLFWwindow* window);
    void uploadScene(const SceneData& scene);
    void verifyUpload(const SceneData& scene);
    bool ensureGeneratedGeometry(bool force = false); // false: reuse cached output
    void verifyGeneratedGeometry(const SceneData& scene);
    void setEdgeSegments(uint32_t segments);
    uint32_t edgeSegments() const { return edgeSegments_; }
    void setDisplacement(const DisplacementSettings& settings);
    void setHeightField(const HeightField& field);
    const HeightField& heightField() const { return compute_->heightField(); }
    uint64_t heightRevision() const { return compute_->heightRevision(); }
    uint64_t heightAllocationBytes() const { return compute_->heightAllocationBytes(); }
    bool supportsWireframe() const { return wireframeSupported_; }
    const GeneratedGeometry& generatedGeometry() const { return compute_->output(); }
    const SceneGpuData& sceneGpuData() const { return sceneGpu_; }
    const SceneUploadStats& uploadStats() const { return uploadStats_; }
    void setClusterSelection(const ClusterSelection& selection);
    const ResolvedClusterSelection& clusterSelection() const { return selection_; }
    const SourceDrawStats& lastRecordedDraw() const { return lastDraw_; }
    const VkPhysicalDeviceProperties& deviceProperties() const { return deviceProperties_; }
    bool validationEnabled() const { return validationEnabled_; }
    bool supportsTimestamps() const { return renderQueries_ != VK_NULL_HANDLE; }
    uint32_t timestampValidBits() const { return timestampValidBits_; }
    VkExtent2D renderExtent() const { return extent_; }
    // Valid only for the most recently submitted measured frame; waits outside the GPU interval.
    std::optional<double> finishRenderMeasurement();
    bool prepareFrame(GLFWwindow* window);
    bool drawFrame(const OrbitCamera& camera, bool clusterColors, bool showBounds, int boundsCluster,
                   bool computeGeometry, bool wireframe, ImDrawData* ui, bool measure = false);

private:
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize allocationBytes = 0;
    };

    struct SwapchainImage {
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkSemaphore renderFinished = VK_NULL_HANDLE;
    };

    void createInstance();
    void createDevice();
    void createFrameResources();
    VkDeviceMemory allocateMemory(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties);
    void createBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    void destroyBuffer(const Buffer& buffer);
    void selectDepthFormat();
    void createDepthResources();
    void recreateSwapchain(int width, int height);
    void createRenderPass();
    void createPipeline();
    void destroySwapchain();
    void initializeUi(uint32_t minImageCount, uint32_t imageCount);
    void shutdownUi();
    void recordDraw(uint32_t imageIndex, const OrbitCamera& camera,
                    bool clusterColors, bool showBounds, int boundsCluster, bool computeGeometry, bool wireframe, ImDrawData* ui, bool measure);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties deviceProperties_{};
    bool validationEnabled_ = false;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    uint32_t presentFamily_ = 0;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<SwapchainImage> swapchainImages_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline wireframePipeline_ = VK_NULL_HANDLE;
    bool wireframeSupported_ = false;
    VkPipeline boundsPipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool scenePool_ = VK_NULL_HANDLE;
    uint32_t uiImageCount_ = 0;
    uint32_t uiMinImageCount_ = 0;
    int framebufferWidth_ = 0;
    int framebufferHeight_ = 0;
    bool rebuildSwapchain_ = true;

    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;

    Buffer meshBuffer_;
    Buffer clusterBuffer_;
    SceneGpuData sceneGpu_;
    SceneUploadStats uploadStats_;
    std::vector<Cluster> drawClusters_;
    ResolvedClusterSelection selection_;
    SourceDrawStats lastDraw_;
    std::unique_ptr<ComputeGeometry> compute_;
    uint32_t edgeSegments_ = 1;
    DisplacementSettings displacement_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkFence frameFence_ = VK_NULL_HANDLE;
    VkQueryPool uploadQueries_ = VK_NULL_HANDLE;
    VkQueryPool renderQueries_ = VK_NULL_HANDLE;
    bool renderMeasurementPending_ = false;
    uint32_t timestampValidBits_ = 0;
    float timestampPeriod_ = 0.0f;
};
