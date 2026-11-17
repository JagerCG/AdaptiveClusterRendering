#pragma once

#include <vulkan/vulkan.h>
#include "SceneGpuData.h"
#include "ComputeGeometry.h"
#include "RenderPlanning.h"

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
    uint32_t rejectedBoundsInstances = 0;
    float boundsPadding = 0;
    uint64_t shadowTriangles = 0;
    uint32_t shadowDrawCalls = 0, visibleClusters = 0;
    RenderDisplay display = RenderDisplay::Shaded;
    bool shadowPass = false;
};

struct LodSelectionStats {
    std::vector<uint32_t> clustersPerLevel;
    double cpuMs = 0;
    std::optional<double> gpuMs;
};

struct VisibilityStats {
    uint32_t candidates = 0, visible = 0, frustum = 0, hierarchy = 0, occluded = 0;
    uint32_t testedNodes = 0, skippedNodes = 0, mipCount = 0;
    uint64_t depthFrame = 0;
    double cpuMs = 0;
    std::optional<double> gpuCullMs, gpuPyramidMs;
};

// Most recently submitted main pass. Submit consumers on the same graphics/compute
// queue before the next draw, or wait for its fence before a host readback.
struct CameraDepthView {
    static constexpr VkFormat format = VK_FORMAT_D32_SFLOAT;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkExtent2D extent{};
    glm::mat4 viewProjection{1};
    uint64_t frame = 0;
    SceneRevision sceneRevision = 0;
    bool solid = false;
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
    void setHierarchyLevel(uint32_t level);
    void selectLod(const OrbitCamera& camera, float padding = 0, bool force = false);
    void selectVisibility(const OrbitCamera& camera, bool generated, bool wireframe);
    const VisibilityStats& visibilityStats() const { return visStats_; }
    const ResolvedClusterSelection& mainSelection() const { return visibilityActive_ ? visibleSelection_ : selection_; }
    const SceneGpuData& mainScene() const { return visibilityActive_ ? visibleScene_ : sceneGpu_; }
    std::vector<float> readHiZ(); // all valid mips in order for acceptance
    const LodSelectionStats& lodStats() const { return lodStats_; }
    const CameraDepthView& cameraDepth() const { return cameraDepth_; }
    std::vector<float> readCameraDepth();
    void checkCameraDepthSampling(); // acceptance probe through the public sampled-image handoff
    bool ensureGeneratedGeometry(bool force = false); // false: reuse cached output
    void verifyGeneratedGeometry(const SceneData& scene);
    void setEdgeSegments(uint32_t segments);
    uint32_t edgeSegments() const { return edgeSegments_; }
    void setDisplacement(const DisplacementSettings& settings);
    void setHeightField(const HeightField& field);
    const HeightField& heightField() const { return compute_->heightField(); }
    uint64_t heightRevision() const { return compute_->heightRevision(); }
    uint64_t heightAllocationBytes() const { return compute_->heightAllocationBytes(); }
    float plannedDisplacementBound() const { return compute_->maxDisplacement(displacement_); }
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
    std::optional<double> lastMainMs() const { return mainMs_; }
    std::optional<double> lastShadowMs() const { return shadowMs_; }
    void setRenderSettings(const RenderSettings& settings);
    const RenderSettings& renderSettings() const { return renderSettings_; }
    // Readbacks are validation only, outside measured intervals.
    std::vector<float> readShadowDepth();
    const glm::mat4& lastLightMatrix() const { return lightMatrix_; }
    static constexpr uint32_t shadowMapSize = 1024;
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
    void createLodResources();
    void createVisibilityResources();
    void destroyVisibilityResources();
    void growVisibilityBuffer(Buffer& buffer, VkDeviceSize& capacity, VkDeviceSize bytes,
                              VkBufferUsageFlags usage, VkMemoryPropertyFlags memory);
    bool generateFor(ComputeGeometry& generator, const SceneGpuData& source, const ResolvedClusterSelection& input, bool force);
    void sizeLodBuffers(VkDeviceSize resultBytes, VkDeviceSize workBytes);
    std::vector<float> readDepth(VkImage image, VkExtent2D extent, VkImageLayout layout);
    VkDeviceMemory allocateMemory(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties);
    void createBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    void destroyBuffer(const Buffer& buffer);
    void selectDepthFormat();
    void createDepthResources();
    void createShadowResources();
    void destroyShadowResources();
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
    uint32_t indirectDrawLimit_ = 1;
    VkPipeline boundsPipeline_ = VK_NULL_HANDLE;
    VkPipeline rejectedBoundsPipeline_ = VK_NULL_HANDLE;
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE, shadowDebugPipeline_ = VK_NULL_HANDLE;
    VkPipeline cameraDebugPipeline_ = VK_NULL_HANDLE;
    VkPipeline hiZDebugPipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout cameraDepthLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet cameraDepthSet_ = VK_NULL_HANDLE;
    CameraDepthView cameraDepth_;
    uint64_t frameSerial_ = 0;
    VkRenderPass shadowRenderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadowLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet shadowSet_ = VK_NULL_HANDLE;
    VkImage shadowImage_ = VK_NULL_HANDLE;
    VkDeviceMemory shadowMemory_ = VK_NULL_HANDLE;
    VkImageView shadowView_ = VK_NULL_HANDLE;
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer_ = VK_NULL_HANDLE;
    Buffer lightingBuffer_;
    RenderSettings renderSettings_;
    glm::mat4 lightMatrix_{1};
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
    std::vector<Cluster> residentClusters_;
    std::vector<LodLevel> residentLevels_;
    std::vector<uint32_t> clusterGroups_; // absolute cluster ID -> parent group for coloring
    std::vector<uint32_t> clusterLevels_;
    struct LodPush {
        glm::vec4 cameraZ{};
        float focalPixels = 0, budget = 0, nearPlane = 0, padding = 0;
        uint32_t nodeWord = 0, nodes = 0, blocks = 0, gridX = 0;
        uint32_t clusterCapacity = 0, triangleCapacity = 0, phase = 0;
    };
    LodPush lastLod_{};
    LodSelectionStats lodStats_;
    Buffer lodResult_, lodWork_;
    VkDeviceSize lodResultSize_ = 0, lodWorkSize_ = 0;
    VkDescriptorSetLayout lodSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet lodSet_ = VK_NULL_HANDLE;
    VkDescriptorPool lodPool_ = VK_NULL_HANDLE;
    VkPipelineLayout lodLayout_ = VK_NULL_HANDLE;
    VkPipeline lodPipeline_ = VK_NULL_HANDLE;
    VkQueryPool lodQueries_ = VK_NULL_HANDLE;
    ResolvedClusterSelection selection_;
    SceneGpuData visibleScene_;
    ResolvedClusterSelection visibleSelection_;
    bool visibilityActive_ = false, separateShadowGeometry_ = false;
    uint64_t candidateRevision_ = 0;
    struct DepthInputs {
        uint64_t candidates = 0, height = 0;
        uint32_t segments = 0;
        DisplacementSettings displacement{};
        bool generated = false;
    } depthInputs_;
    VisibilityStats visStats_;
    struct Mip { uint32_t width, height, offset; };
    std::vector<Mip> hiZMips_;
    Buffer hiZ_, visInput_, visResult_, visWork_;
    VkDeviceSize hiZCapacity_ = 0, visInputCapacity_ = 0, visResultCapacity_ = 0, visWorkCapacity_ = 0;
    VkDescriptorSetLayout visSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool visPool_ = VK_NULL_HANDLE;
    VkDescriptorSet visSet_ = VK_NULL_HANDLE;
    VkPipelineLayout visLayout_ = VK_NULL_HANDLE;
    VkPipeline hiZBuild_ = VK_NULL_HANDLE, visCull_ = VK_NULL_HANDLE;
    VkQueryPool visQueries_ = VK_NULL_HANDLE;
    std::unique_ptr<ComputeGeometry> shadowCompute_;
    uint64_t shadowHeightRevision_ = 0;
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
    std::optional<double> mainMs_, shadowMs_;
    uint32_t timestampValidBits_ = 0;
    float timestampPeriod_ = 0.0f;
};
