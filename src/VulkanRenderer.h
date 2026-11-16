#pragma once

#include <vulkan/vulkan.h>
#include "SceneGpuData.h"

#include <glm/mat4x4.hpp>
#include <vector>

struct GLFWwindow;
struct ClusterDataset;
struct ImDrawData;
struct MeshSource;
struct SceneData;
class OrbitCamera;

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void initialize(GLFWwindow* window);
    void uploadScene(const SceneData& scene);
    void verifyUpload(const SceneData& scene);
    const SceneGpuData& sceneGpuData() const { return sceneGpu_; }
    bool prepareFrame(GLFWwindow* window);
    bool drawFrame(const OrbitCamera& camera, bool clusterColors, bool showBounds, int boundsCluster,
                   bool lodEnabled, bool cullingEnabled, float lodError, int cullingMode,
                   bool shadowsEnabled, bool shadowMapView, float shadowBias,
                   bool wireframe, bool depthDisplay, int drawMode, ImDrawData* ui);

private:
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
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
                    bool clusterColors, bool showBounds, int boundsCluster,
                    bool shadowsEnabled, bool shadowMapView, float shadowBias,
                    bool wireframe, bool depthDisplay, int drawMode, ImDrawData* ui);
    void createShadowResources();
    void destroyShadowResources();
    void createShadowRenderPass();
    void createShadowPipeline();
    glm::mat4 buildShadowViewProjection(const OrbitCamera& camera) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
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
    VkRenderPass shadowRenderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline wireframePipeline_ = VK_NULL_HANDLE;
    VkPipeline boundsPipeline_ = VK_NULL_HANDLE;
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadowLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool scenePool_ = VK_NULL_HANDLE;
    VkDescriptorSet shadowSet_ = VK_NULL_HANDLE;
    VkImage shadowImage_ = VK_NULL_HANDLE;
    VkDeviceMemory shadowMemory_ = VK_NULL_HANDLE;
    VkImageView shadowView_ = VK_NULL_HANDLE;
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer_ = VK_NULL_HANDLE;
    uint32_t shadowMapSize_ = 1024;
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
    std::vector<Cluster> sourceClusters_;
    std::vector<Cluster> drawClusters_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkFence frameFence_ = VK_NULL_HANDLE;
};
