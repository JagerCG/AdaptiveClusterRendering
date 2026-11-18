#include "VulkanRenderer.h"

#include "ClusterBuilder.h"
#include "MeshSource.h"
#include "OrbitCamera.h"
#include "SceneData.h"
#include "GeometryValidation.h"

#include <GLFW/glfw3.h>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec4.hpp>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

constexpr uint32_t kVulkanVersion = VK_API_VERSION_1_2;
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr uint32_t kInvalidFamily = std::numeric_limits<uint32_t>::max();
constexpr uint64_t kWaitForever = std::numeric_limits<uint64_t>::max();
constexpr uint64_t kAcquireTimeoutNs = 100'000'000;
constexpr glm::vec4 kShadedColor{0.24f, 0.54f, 0.84f, 1.0f};

static_assert(std::is_standard_layout_v<Vertex> && std::is_trivially_copyable_v<Vertex>);
static_assert(sizeof(glm::mat4) == 16 * sizeof(float));
static_assert(sizeof(glm::vec4) == 4 * sizeof(float));

constexpr uint32_t kVertexShader[] = {
#include "mesh.vert.inc"
};
constexpr uint32_t kFragmentShader[] = {
#include "mesh.frag.inc"
};
struct DrawPush {
    glm::mat4 viewProjection{1};
    glm::vec4 baseColor = kShadedColor;
    float boundsPadding = 0;
    uint32_t displayMode = 0, shadows = 0;
    float shadowBias = 0;
};
static_assert(sizeof(DrawPush) == 96 && offsetof(DrawPush, boundsPadding) == 80 && offsetof(DrawPush, shadowBias) == 92);
struct LightingUniform { glm::mat4 lightViewProjection; glm::vec4 lightDirection; };
static_assert(sizeof(LightingUniform) == 80);
constexpr uint32_t kShadowVertexShader[] = {
#include "shadow.vert.inc"
};
constexpr uint32_t kShadowFragmentShader[] = {
#include "shadow.frag.inc"
};
constexpr uint32_t kShadowDebugVertexShader[] = {
#include "shadow_debug.vert.inc"
};
constexpr uint32_t kShadowDebugFragmentShader[] = {
#include "shadow_debug.frag.inc"
};
constexpr uint32_t kCameraDepthFragmentShader[] = {
#include "camera_depth.frag.inc"
};
constexpr uint32_t kBoundsVertexShader[] = {
#include "bounds.vert.inc"
};
constexpr uint32_t kBoundsFragmentShader[] = {
#include "bounds.frag.inc"
};

void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        const std::string message = std::string(operation) + " failed (VkResult " + std::to_string(result) + ")";
        throw std::runtime_error(message);
    }
}

glm::vec4 clusterColor(uint32_t id)
{
    // Spread consecutive IDs around the hue wheel.
    const uint32_t hueBits = id * 2654435761u;
    const float hue = static_cast<float>(hueBits >> 8) / 16777216.0f;
    const glm::vec3 cycle = glm::fract(glm::vec3(hue) + glm::vec3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f));
    const glm::vec3 rgb = glm::clamp(glm::abs(cycle * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
    return glm::vec4(0.22f + 0.73f * rgb, 1.0f);
}

VKAPI_ATTR VkBool32 VKAPI_CALL printValidationMessage(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* message, void*)
{
    const char* level = severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning";
    std::cerr << "Vulkan validation " << level << ": " << message->pMessage << '\n';
    return VK_FALSE;
}

VkShaderModule createShaderModule(VkDevice device, const uint32_t* code, size_t size)
{
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule shader = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &info, nullptr, &shader), "vkCreateShaderModule");
    return shader;
}

} // namespace

VulkanRenderer::~VulkanRenderer()
{
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        compute_.reset(); // owns device resources; destroy before vkDestroyDevice
        shadowCompute_.reset();
        shutdownUi();
        destroySwapchain();
        destroyShadowResources();
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipeline(device_, wireframePipeline_, nullptr);
        vkDestroyPipeline(device_, boundsPipeline_, nullptr);
        vkDestroyPipeline(device_, shadowPipeline_, nullptr);
        vkDestroyPipeline(device_, shadowDebugPipeline_, nullptr);
        vkDestroyPipeline(device_, cameraDebugPipeline_, nullptr);
        destroyShadowCasterResources();
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        vkDestroyDescriptorPool(device_, scenePool_, nullptr);
        vkDestroyDescriptorSetLayout(device_, sceneLayout_, nullptr);
        vkDestroyDescriptorSetLayout(device_, shadowLayout_, nullptr);
        vkDestroyDescriptorSetLayout(device_, cameraDepthLayout_, nullptr);
        vkDestroyRenderPass(device_, shadowRenderPass_, nullptr);
        destroyBuffer(lightingBuffer_);
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        vkDestroyFence(device_, frameFence_, nullptr);
        vkDestroySemaphore(device_, imageAvailable_, nullptr);
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyQueryPool(device_, uploadQueries_, nullptr);
        vkDestroyQueryPool(device_, renderQueries_, nullptr);
        destroyBuffer(meshBuffer_);
        destroyBuffer(clusterBuffer_);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (debugMessenger_ != VK_NULL_HANDLE) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        destroyMessenger(instance_, debugMessenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

void VulkanRenderer::initialize(GLFWwindow* window)
{
    createInstance();
    check(glfwCreateWindowSurface(instance_, window, nullptr, &surface_), "glfwCreateWindowSurface");
    createDevice();
    selectDepthFormat();
    createFrameResources();
    createShadowCasterResources();
    createShadowResources();
    compute_ = std::make_unique<ComputeGeometry>();
    compute_->initialize(gpu_, device_, graphicsFamily_);
    setHeightField(makeHeightField(HeightPreset::Waves));
}

bool VulkanRenderer::prepareFrame(GLFWwindow* window)
{
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width == 0 || height == 0 || glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
        return false;
    }
    if (rebuildSwapchain_ || width != framebufferWidth_ || height != framebufferHeight_) {
        recreateSwapchain(width, height);
        framebufferWidth_ = width;
        framebufferHeight_ = height;
        rebuildSwapchain_ = false;
    }
    return true;
}

void VulkanRenderer::createInstance()
{
    uint32_t extensionCount = 0;
    const char** requiredExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
    if (requiredExtensions == nullptr) {
        throw std::runtime_error("GLFW could not find Vulkan window extensions.");
    }
    std::vector<const char*> extensions(requiredExtensions, requiredExtensions + extensionCount);
    bool enableValidation = false;

#ifndef NDEBUG
    uint32_t layerCount = 0;
    check(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "vkEnumerateInstanceLayerProperties");
    std::vector<VkLayerProperties> layers(layerCount);
    check(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "vkEnumerateInstanceLayerProperties");
    enableValidation = std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, kValidationLayer) == 0;
    });
    std::cout << "Vulkan validation: " << (enableValidation ? "enabled" : "layer unavailable") << std::endl;
#endif

    VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
    if (enableValidation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = printValidationMessage;
    }

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Adaptive Cluster Rendering";
    application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application.apiVersion = kVulkanVersion;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    if (enableValidation) {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &kValidationLayer;
        info.pNext = &debugInfo;
    }
    check(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
    validationEnabled_ = enableValidation;

    if (enableValidation) {
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger == nullptr) {
            throw std::runtime_error("Could not load vkCreateDebugUtilsMessengerEXT.");
        }
        check(createMessenger(instance_, &debugInfo, nullptr, &debugMessenger_), "vkCreateDebugUtilsMessengerEXT");
    }
}

void VulkanRenderer::createDevice()
{
    uint32_t gpuCount = 0;
    check(vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(gpuCount);
    check(vkEnumeratePhysicalDevices(instance_, &gpuCount, devices.data()), "vkEnumeratePhysicalDevices");

    int bestScore = 0;
    VkPhysicalDeviceProperties selectedProperties{};
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < kVulkanVersion) {
            continue;
        }
        if (properties.limits.maxComputeWorkGroupInvocations < kGeometryGroupSize ||
            properties.limits.maxComputeWorkGroupSize[0] < kGeometryGroupSize ||
            properties.limits.maxPerStageDescriptorStorageBuffers < 5 ||
            properties.limits.maxDescriptorSetStorageBuffers < 5 || properties.limits.maxPushConstantsSize < 84 ||
            properties.limits.maxPerStageDescriptorSamplers < 1 || properties.limits.maxPerStageDescriptorSampledImages < 1) {
            continue;
        }

        uint32_t extensionCount = 0;
        check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr),
              "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(extensionCount);
        check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data()),
              "vkEnumerateDeviceExtensionProperties");
        const bool hasSwapchain = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        });
        if (!hasSwapchain) {
            continue;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        uint32_t graphics = kInvalidFamily;
        uint32_t present = kInvalidFamily;
        for (uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 canPresent = VK_FALSE;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &canPresent),
                  "vkGetPhysicalDeviceSurfaceSupportKHR");
            const auto required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            const bool canDraw = (families[i].queueFlags & required) == required;
            if (canDraw && canPresent) {
                graphics = i;
                present = i;
                break;
            }
            if (canDraw && graphics == kInvalidFamily) {
                graphics = i;
            }
            if (canPresent && present == kInvalidFamily) {
                present = i;
            }
        }
        if (graphics == kInvalidFamily || present == kInvalidFamily) {
            continue;
        }

        uint32_t formatCount = 0;
        uint32_t modeCount = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface_, &formatCount, nullptr),
              "vkGetPhysicalDeviceSurfaceFormatsKHR");
        check(vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface_, &modeCount, nullptr),
              "vkGetPhysicalDeviceSurfacePresentModesKHR");
        if (formatCount == 0 || modeCount == 0) {
            continue;
        }

        const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            gpu_ = candidate;
            graphicsFamily_ = graphics;
            presentFamily_ = present;
            selectedProperties = properties;
            timestampValidBits_ = families[graphics].timestampValidBits;
            timestampPeriod_ = properties.limits.timestampPeriod;
        }
    }
    if (gpu_ == VK_NULL_HANDLE) {
        throw std::runtime_error("No Vulkan 1.2 device supports this window and the geometry compute pipeline.");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queues[2]{};
    queues[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queues[0].queueFamilyIndex = graphicsFamily_;
    queues[0].queueCount = 1;
    queues[0].pQueuePriorities = &priority;
    queues[1] = queues[0];
    queues[1].queueFamilyIndex = presentFamily_;

    const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = graphicsFamily_ == presentFamily_ ? 1u : 2u;
    info.pQueueCreateInfos = queues;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &swapchainExtension;
    VkPhysicalDeviceFeatures available{}, enabled{};
    vkGetPhysicalDeviceFeatures(gpu_, &available);
    enabled.fillModeNonSolid = available.fillModeNonSolid;
    wireframeSupported_ = available.fillModeNonSolid == VK_TRUE;
    enabled.multiDrawIndirect = available.multiDrawIndirect;
    indirectDrawLimit_ = available.multiDrawIndirect ? selectedProperties.limits.maxDrawIndirectCount : 1;
    info.pEnabledFeatures = &enabled;
    check(vkCreateDevice(gpu_, &info, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
    std::cout << "Rendering on " << selectedProperties.deviceName << std::endl;
    deviceProperties_ = selectedProperties;
}

void VulkanRenderer::createFrameResources()
{
    if (timestampValidBits_ > 0 && timestampPeriod_ > 0.0f) {
        VkQueryPoolCreateInfo queries{};
        queries.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queries.queryCount = 2;
        check(vkCreateQueryPool(device_, &queries, nullptr, &uploadQueries_), "vkCreateQueryPool");
        queries.queryCount = 3;
        check(vkCreateQueryPool(device_, &queries, nullptr, &renderQueries_), "vkCreateQueryPool(render)");
    }
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsFamily_;
    check(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device_, &commandInfo, &commandBuffer_), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(device_, &fenceInfo, nullptr, &frameFence_), "vkCreateFence");

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_), "vkCreateSemaphore");

    const VkDescriptorSetLayoutBinding clusterBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sceneLayout{};
    sceneLayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sceneLayout.bindingCount = 1;
    sceneLayout.pBindings = &clusterBinding;
    check(vkCreateDescriptorSetLayout(device_, &sceneLayout, nullptr, &sceneLayout_), "vkCreateDescriptorSetLayout");
    const VkDescriptorSetLayoutBinding shadowBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}
    };
    VkDescriptorSetLayoutCreateInfo shadowLayout{};
    shadowLayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    shadowLayout.bindingCount = 2; shadowLayout.pBindings = shadowBindings;
    check(vkCreateDescriptorSetLayout(device_, &shadowLayout, nullptr, &shadowLayout_), "vkCreateDescriptorSetLayout(shadow)");
    const VkDescriptorSetLayoutBinding cameraBinding{0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo cameraLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    cameraLayout.bindingCount = 1; cameraLayout.pBindings = &cameraBinding;
    check(vkCreateDescriptorSetLayout(device_, &cameraLayout, nullptr, &cameraDepthLayout_), "Create camera depth layout");
    const VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1}};
    VkDescriptorPoolCreateInfo scenePool{};
    scenePool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    scenePool.maxSets = 3; scenePool.poolSizeCount = 4; scenePool.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(device_, &scenePool, nullptr, &scenePool_), "vkCreateDescriptorPool");
    const VkDescriptorSetLayout setLayouts[] = {sceneLayout_, shadowLayout_, cameraDepthLayout_};
    VkDescriptorSet sets[3]{};
    VkDescriptorSetAllocateInfo descriptor{};
    descriptor.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor.descriptorPool = scenePool_; descriptor.descriptorSetCount = 3; descriptor.pSetLayouts = setLayouts;
    check(vkAllocateDescriptorSets(device_, &descriptor, sets), "vkAllocateDescriptorSets");
    sceneGpu_.clusterSet = sets[0]; shadowSet_ = sets[1]; cameraDepthSet_ = sets[2];
    createBuffer(lightingBuffer_, sizeof(LightingUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 3;
    layoutInfo.pSetLayouts = setLayouts;
    const VkPushConstantRange ranges[] = {
        {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawPush)}
    };
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = ranges;
    check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");
}

VkDeviceMemory VulkanRenderer::allocateMemory(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(gpu_, &memoryProperties);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const auto flags = memoryProperties.memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1u << i)) == 0 || (flags & properties) != properties) {
            continue;
        }

        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = i;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        check(vkAllocateMemory(device_, &allocation, nullptr, &memory), "vkAllocateMemory");
        return memory;
    }
    throw std::runtime_error("No compatible Vulkan memory type.");
}

void VulkanRenderer::createBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags properties)
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &info, nullptr, &buffer.handle), "vkCreateBuffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer.handle, &requirements);
    buffer.memory = allocateMemory(requirements, properties);
    buffer.allocationBytes = requirements.size;
    check(vkBindBufferMemory(device_, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");
}

void VulkanRenderer::destroyBuffer(const Buffer& buffer)
{
    vkDestroyBuffer(device_, buffer.handle, nullptr);
    vkFreeMemory(device_, buffer.memory, nullptr);
}

namespace {
struct PackedSource {
    std::vector<uint32_t> indices, records;
    ResidentHierarchy layout;
};

PackedSource packSource(const SceneData& scene)
{
    PackedSource result;
    const auto& h = scene.hierarchy;
    result.indices = scene.dataset.indices;
    result.indices.insert(result.indices.end(), h.coarse.indices.begin(), h.coarse.indices.end());
    const auto append = [&](const auto& values) {
        const size_t start = result.records.size();
        const size_t bytes = values.size() * sizeof(values[0]);
        if (bytes) {
            result.records.resize(start + bytes / sizeof(uint32_t));
            std::memcpy(result.records.data() + start, values.data(), bytes);
        }
        return static_cast<uint32_t>(start);
    };
    append(packClusters(scene.dataset));
    append(packClusters(h.coarse, static_cast<uint32_t>(scene.dataset.clusters.size())));
    auto& view = result.layout;
    view.allIndexOffset = scene.mesh.vertices.size() * sizeof(Vertex);
    view.totalIndices = static_cast<uint32_t>(result.indices.size());
    view.totalClusters = static_cast<uint32_t>(scene.dataset.clusters.size() + h.coarse.clusters.size());
    view.levelWord = append(h.levels);
    view.nodeWord = append(h.nodes);
    view.childWord = append(h.children);
    view.levelCount = static_cast<uint32_t>(h.levels.size());
    view.nodeCount = static_cast<uint32_t>(h.nodes.size());
    view.childCount = static_cast<uint32_t>(h.children.size());
    return result;
}
} // namespace

void VulkanRenderer::uploadScene(const SceneData& scene)
{
    const auto cpuStart = std::chrono::steady_clock::now();
    SceneUploadStats nextStats;
    validateScene(scene);
    if (sceneGpu_.sceneRevision == std::numeric_limits<SceneRevision>::max()) {
        throw std::runtime_error("Source scene revision exhausted.");
    }
    const SceneRevision nextRevision = sceneGpu_.sceneRevision + 1;
    auto nextSelection = resolveClusterSelection(scene.dataset.clusters,
        static_cast<uint32_t>(scene.dataset.indices.size()), nextRevision,
        {nextRevision, ClusterSelectionMode::All, {}});
    nextStats.payload = scenePayloadBytes(scene);
    const auto& mesh = scene.mesh;
    const auto packed = packSource(scene);
    const auto& indices = packed.indices;
    auto drawClusters = scene.dataset.clusters;
    auto allClusters = drawClusters;
    allClusters.insert(allClusters.end(), scene.hierarchy.coarse.clusters.begin(),
                       scene.hierarchy.coarse.clusters.end());
    auto levels = scene.hierarchy.levels;
    if (levels.empty()) levels.push_back(sceneLevel(scene, 0));
    const size_t vertexBytes = mesh.vertices.size() * sizeof(Vertex);
    const size_t indexBytes = indices.size() * sizeof(uint32_t);
    const size_t meshBytes = vertexBytes + indexBytes;
    const size_t clusterBytes = packed.records.size() * sizeof(uint32_t);
    const VkDeviceSize bufferSize = meshBytes + clusterBytes;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu_, &properties);
    if (clusterBytes > properties.limits.maxStorageBufferRange) {
        throw std::runtime_error("Cluster and hierarchy buffer exceeds this GPU's maxStorageBufferRange.");
    }
    if (!scene.hierarchy.levels.empty() && meshBytes > properties.limits.maxStorageBufferRange)
        throw std::runtime_error("Resident multi-level geometry exceeds this GPU's storage-buffer range.");
    const auto storageUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const auto meshUsage = storageUsage | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, kWaitForever), "vkWaitForFences");
    Buffer replacement;
    Buffer replacementClusters;
    Buffer staging;
    try {
        createBuffer(replacement, meshBytes, meshUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        createBuffer(replacementClusters, clusterBytes, storageUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const auto stagingMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        createBuffer(staging, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stagingMemory);
        void* mapped = nullptr;
        check(vkMapMemory(device_, staging.memory, 0, bufferSize, 0, &mapped), "vkMapMemory");
        auto* bytes = static_cast<char*>(mapped);
        std::memcpy(bytes, mesh.vertices.data(), vertexBytes);
        std::memcpy(bytes + vertexBytes, indices.data(), indexBytes);
        std::memcpy(bytes + meshBytes, packed.records.data(), clusterBytes);
        vkUnmapMemory(device_, staging.memory);

        check(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");
        if (uploadQueries_ != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(commandBuffer_, uploadQueries_, 0, 2);
            vkCmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, uploadQueries_, 0);
        }
        const VkBufferCopy copy{0, 0, meshBytes};
        vkCmdCopyBuffer(commandBuffer_, staging.handle, replacement.handle, 1, &copy);
        const VkBufferCopy clusterCopy{meshBytes, 0, clusterBytes};
        vkCmdCopyBuffer(commandBuffer_, staging.handle, replacementClusters.handle, 1, &clusterCopy);
        if (uploadQueries_ != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, uploadQueries_, 1);
        }

        VkBufferMemoryBarrier barriers[2]{};
        for (auto& barrier : barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.size = VK_WHOLE_SIZE;
        }
        barriers[0].buffer = replacement.handle;
        barriers[0].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        barriers[1].buffer = replacementClusters.handle;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 2, barriers, 0, nullptr);
        check(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        check(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        check(vkQueueWaitIdle(graphicsQueue_), "vkQueueWaitIdle");
        if (uploadQueries_ != VK_NULL_HANDLE) {
            uint64_t timestamps[2]{};
            check(vkGetQueryPoolResults(device_, uploadQueries_, 0, 2, sizeof(timestamps), timestamps,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                  "vkGetQueryPoolResults");
            // Unsigned subtraction also handles a single wrap at the queue's valid width.
            const uint64_t mask = timestampValidBits_ == 64 ? ~uint64_t{0} :
                                  (uint64_t{1} << timestampValidBits_) - 1;
            nextStats.gpuTransferMs = double((timestamps[1] - timestamps[0]) & mask) *
                                      double(timestampPeriod_) / 1'000'000.0;
        }
    } catch (...) {
        destroyBuffer(staging);
        destroyBuffer(replacement);
        destroyBuffer(replacementClusters);
        throw;
    }
    destroyBuffer(staging);
    destroyBuffer(meshBuffer_);
    destroyBuffer(clusterBuffer_);
    meshBuffer_ = replacement;
    clusterBuffer_ = replacementClusters;
    sceneGpu_.vertexBuffer = sceneGpu_.indexBuffer = meshBuffer_.handle;
    sceneGpu_.clusterBuffer = clusterBuffer_.handle;
    sceneGpu_.indexOffset = vertexBytes;
    sceneGpu_.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    sceneGpu_.indexCount = levels[0].indexCount;
    sceneGpu_.clusterCount = levels[0].clusterCount;
    sceneGpu_.hierarchy = packed.layout;
    sceneGpu_.activeLevel = sceneGpu_.firstCluster = 0;
    lightList_.active = separateShadowGeometry_ = false;
    lightList_.stats = {};
    ++candidateRevision_;
    cameraDepth_ = {};
    residentClusters_ = std::move(allClusters);
    residentLevels_ = std::move(levels);
    drawClusters_ = std::move(drawClusters);
    const VkDescriptorBufferInfo clusterInfo{clusterBuffer_.handle, 0, clusterBytes};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sceneGpu_.clusterSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &clusterInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    nextStats.residentAllocationBytes = meshBuffer_.allocationBytes + clusterBuffer_.allocationBytes;
    nextStats.cpuUploadMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - cpuStart).count();
    uploadStats_ = nextStats;
    sceneGpu_.sceneRevision = nextRevision;
    selection_ = std::move(nextSelection);
    compute_->invalidate();
}

void VulkanRenderer::setHierarchyLevel(uint32_t level)
{
    const auto& l = residentLevels_.at(level);
    if (sceneGpu_.activeLevel == level) return;
    if (sceneGpu_.sceneRevision == std::numeric_limits<SceneRevision>::max())
        throw std::runtime_error("Scene revision exhausted.");
    const auto revision = sceneGpu_.sceneRevision + 1;
    auto begin = residentClusters_.begin() + l.firstCluster;
    std::vector<Cluster> active(begin, begin + l.clusterCount);
    for (auto& c : active)
        c.firstIndex -= l.firstIndex;
    auto selection =
        resolveClusterSelection(active, l.indexCount, revision, {revision, ClusterSelectionMode::All, {}});
    sceneGpu_.activeLevel = level;
    lightList_.active = false;
    ++candidateRevision_;
    sceneGpu_.firstCluster = l.firstCluster;
    sceneGpu_.indexOffset = sceneGpu_.hierarchy.allIndexOffset + VkDeviceSize(l.firstIndex) * sizeof(uint32_t);
    sceneGpu_.indexCount = l.indexCount;
    sceneGpu_.clusterCount = l.clusterCount;
    sceneGpu_.sceneRevision = revision;
    drawClusters_ = std::move(active);
    selection_ = std::move(selection);
    compute_->invalidate();
}

void VulkanRenderer::setClusterSelection(const ClusterSelection& selection)
{
    // Resolve first so a rejected request preserves the active selection.
    auto next = resolveClusterSelection(drawClusters_, sceneGpu_.indexCount, sceneGpu_.sceneRevision, selection);
    if (!sameClusterSelection(selection_, next)) {
        compute_->invalidate();
        lightList_.active = false;
        ++candidateRevision_;
    }
    selection_ = std::move(next);
}

bool VulkanRenderer::ensureGeneratedGeometry(bool force)
{
    const bool changed = generateFor(*compute_, sceneGpu_, selection_, force);
    prepareShadowPass(true, force);
    return changed;
}

void VulkanRenderer::prepareShadowPass(bool generated, bool forceCulling)
{
    const float padding = generated ? plannedDisplacementBound() : 0.f;
    const bool refit = lightFitRevision_ != sceneGpu_.sceneRevision || lightPadding_ != padding;
    if (refit) {
        lightMatrix_ = shadowViewProjection(residentClusters_, padding, renderSettings_.lightDirection,
                                            renderSettings_.shadowCoverage);
        lightFitRevision_ = sceneGpu_.sceneRevision;
        lightPadding_ = padding;
        lightList_.active = false;
    }
    const bool shadowPass = renderSettings_.shadows || renderSettings_.display == RenderDisplay::ShadowMap;
    if (shadowPass && renderSettings_.shadowCulling) {
        if (forceCulling || !lightList_.active || lightCandidateRevision_ != candidateRevision_) {
            selectShadowCasters(lightMatrix_, padding);
            lightCandidateRevision_ = candidateRevision_;
        }
    } else {
        lightList_.active = false;
        lightList_.stats = {};
    }

    const auto& mainRanges = selection_.ranges;
    const auto& lightRanges = shadowSelection().ranges;
    separateShadowGeometry_ = generated && shadowPass && &mainRanges != &lightRanges &&
        !std::equal(mainRanges.begin(), mainRanges.end(), lightRanges.begin(), lightRanges.end(),
                    [](const auto& a, const auto& b) { return a.sourceClusterId == b.sourceClusterId; });
    if (separateShadowGeometry_) {
        if (!shadowCompute_) {
            auto next = std::make_unique<ComputeGeometry>();
            next->initialize(gpu_, device_, graphicsFamily_);
            shadowCompute_ = std::move(next);
        }
        if (shadowHeightRevision_ != heightRevision()) {
            check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, kWaitForever),
                  "Wait before shadow height upload");
            shadowCompute_->uploadHeight(heightField(), commandBuffer_, graphicsQueue_);
            shadowHeightRevision_ = heightRevision();
        }
        generateFor(*shadowCompute_, shadowScene(), shadowSelection(), false);
    }
}

bool VulkanRenderer::generateFor(ComputeGeometry& generator, const SceneGpuData& source,
                                 const ResolvedClusterSelection& selected, bool force)
{
    if (!force && generator.matches(source, selected, edgeSegments_, displacement_)) return false;
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, kWaitForever), "vkWaitForFences");
    generator.prepare(source, selected, edgeSegments_, displacement_);
    check(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");
    generator.record(commandBuffer_);
    check(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    check(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit (compute)");
    check(vkQueueWaitIdle(graphicsQueue_), "vkQueueWaitIdle (compute)");
    generator.finish();
    return true;
}

void VulkanRenderer::setEdgeSegments(uint32_t segments)
{
    if (!validEdgeSegments(segments)) throw std::runtime_error("Edge segments must be 1, 2, 4 or 8.");
    if (segments != edgeSegments_) compute_->invalidate();
    edgeSegments_ = segments;
}

void VulkanRenderer::setDisplacement(const DisplacementSettings& settings)
{
    validateDisplacement(settings);
    if (!sameDisplacement(displacement_, settings)) compute_->invalidate();
    displacement_ = settings;
}

void VulkanRenderer::setHeightField(const HeightField& field)
{
    validateHeightField(field);
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, kWaitForever), "vkWaitForFences (height)");
    compute_->uploadHeight(field, commandBuffer_, graphicsQueue_);
}

void VulkanRenderer::verifyGeneratedGeometry(const SceneData& scene)
{
    const auto& selected = selection_;
    const auto& output = compute_->output();
    const uint64_t multiplier = uint64_t(edgeSegments_) * edgeSegments_;
    if (!compute_->matches(sceneGpu_, selected, edgeSegments_, displacement_) ||
        output.sourceRevision != sceneGpu_.sceneRevision || output.edgeSegments != edgeSegments_ ||
        output.sourceTriangleCount != selected.triangleCount() ||
        output.vertexCount != selected.indexCount * multiplier ||
        output.indexCount != selected.indexCount * multiplier ||
        output.vertexBytes != uint64_t(output.vertexCount) * sizeof(Vertex) ||
        output.indexBytes != uint64_t(output.indexCount) * sizeof(uint32_t) ||
        output.vertexStride != sizeof(Vertex) || output.indexStride != sizeof(uint32_t) ||
        output.allocatedOutputBytes < output.vertexBytes + output.indexBytes ||
        output.allocatedOutputBytes > kDefaultGeometryOutputBudget ||
        output.worklistBytes != selected.triangleCount() * sizeof(uint32_t) ||
        output.allocatedWorklistBytes < output.worklistBytes || !std::isfinite(output.cpuGenerateMs) ||
        output.cpuGenerateMs < 0 ||
        output.maxDisplacement != displacementBound(compute_->heightField(), displacement_) ||
        (output.gpuGenerateMs && (!std::isfinite(*output.gpuGenerateMs) || *output.gpuGenerateMs < 0)) ||
        output.gpuGenerateMs.has_value() !=
            (timestampValidBits_ > 0 && timestampPeriod_ > 0 && output.indexCount > 0) ||
        output.ranges.size() != selected.ranges.size()) {
        throw std::runtime_error("Generated output metadata / timing does not match the selected source.");
    }
    uint32_t expectedFirst = 0;
    for (size_t i = 0; i < selected.ranges.size(); ++i) {
        const auto& input = selected.ranges[i];
        const auto& range = output.ranges[i];
        if (range.sourceClusterId != input.sourceClusterId || range.firstIndex != expectedFirst ||
            range.indexCount != input.indexCount * multiplier) throw std::runtime_error("Generated draw range lost its source ID / order.");
        expectedFirst += range.indexCount;
    }
    if (!output.indexCount) {
        if (output.vertexBuffer || output.indexBuffer || output.allocatedOutputBytes || output.workgroups ||
            output.allocatedWorklistBytes) throw std::runtime_error("Empty compute output still owns drawable buffers.");
        std::cout << "GPU geometry readback passed: empty selection, zero output / dispatch (n=" << edgeSegments_ << ")." << std::endl;
        return;
    }
    if (!output.vertexBuffer || !output.indexBuffer || output.vertexBuffer == sceneGpu_.vertexBuffer ||
        output.indexBuffer == sceneGpu_.indexBuffer || output.vertexBuffer == output.indexBuffer) {
        throw std::runtime_error("Compute output is not independent of the source buffers.");
    }
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, kWaitForever), "vkWaitForFences");
    Buffer readback;
    const VkDeviceSize total = output.vertexBytes + output.indexBytes;
    try {
        createBuffer(readback, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");
        // ComputeGeometry already made output writes visible to transfer reads.
        const VkBufferCopy vertexCopy{output.vertexOffset, 0, output.vertexBytes};
        const VkBufferCopy indexCopy{output.indexOffset, output.vertexBytes, output.indexBytes};
        vkCmdCopyBuffer(commandBuffer_, output.vertexBuffer, readback.handle, 1, &vertexCopy);
        vkCmdCopyBuffer(commandBuffer_, output.indexBuffer, readback.handle, 1, &indexCopy);
        VkMemoryBarrier after{};
        after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        check(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit (compute readback)");
        check(vkQueueWaitIdle(graphicsQueue_), "vkQueueWaitIdle (compute readback)");
        void* mapped = nullptr;
        check(vkMapMemory(device_, readback.memory, 0, total, 0, &mapped), "vkMapMemory (compute readback)");
        const auto* bytes = static_cast<const char*>(mapped);
        const auto* indices = reinterpret_cast<const uint32_t*>(bytes + output.vertexBytes);
        std::vector<Vertex> vertices(output.vertexCount);
        std::vector<uint32_t> outputIndices(output.indexCount);
        std::memcpy(vertices.data(), bytes, static_cast<size_t>(output.vertexBytes));
        std::memcpy(outputIndices.data(), indices, static_cast<size_t>(output.indexBytes));
        vkUnmapMemory(device_, readback.memory);
        const auto stats = validateGeneratedGeometry(scene, selected, edgeSegments_, vertices, outputIndices,
                                                     &compute_->heightField(), displacement_, sceneGpu_.activeLevel);
        std::cout << "GPU geometry readback passed: n=" << edgeSegments_ << ", " << output.vertexCount << " vertices, "
                  << stats.triangles << " triangles, " << stats.sharedEdgeChecks << " exact shared-edge checks, max attribute error "
                  << stats.maxAttributeError << ", max normal error " << stats.maxNormalError
                  << ", displacement bound " << output.maxDisplacement << "; reference / bounds / indices verified." << std::endl;
    } catch (...) {
        destroyBuffer(readback);
        throw;
    }
    destroyBuffer(readback);
}

void VulkanRenderer::verifyUpload(const SceneData& scene)
{
    const auto expected = scenePayloadBytes(scene);
    if (uploadStats_.payload.vertices != expected.vertices || uploadStats_.payload.indices != expected.indices ||
        uploadStats_.payload.clusters != expected.clusters || uploadStats_.payload.hierarchy != expected.hierarchy ||
        uploadStats_.residentAllocationBytes < expected.total() ||
        !std::isfinite(uploadStats_.cpuUploadMs) || uploadStats_.cpuUploadMs < 0.0 ||
        (uploadStats_.gpuTransferMs &&
         (!std::isfinite(*uploadStats_.gpuTransferMs) || *uploadStats_.gpuTransferMs < 0.0)) ||
        uploadStats_.gpuTransferMs.has_value() != (uploadQueries_ != VK_NULL_HANDLE)) {
        throw std::runtime_error("Upload diagnostics do not match the resident scene / timestamp support.");
    }
    const auto packed = packSource(scene);
    const size_t vertexBytes = scene.mesh.vertices.size() * sizeof(Vertex);
    const size_t indexBytes = packed.indices.size() * sizeof(uint32_t);
    const size_t meshBytes = vertexBytes + indexBytes;
    const size_t clusterBytes = packed.records.size()*sizeof(uint32_t);
    const auto active = sceneLevel(scene, sceneGpu_.activeLevel);
    if (sceneGpu_.vertexCount != scene.mesh.vertices.size() || sceneGpu_.indexCount != active.indexCount ||
        sceneGpu_.clusterCount != active.clusterCount || sceneGpu_.firstCluster != active.firstCluster ||
        sceneGpu_.indexOffset != vertexBytes + VkDeviceSize(active.firstIndex)*4)
        throw std::runtime_error("GPU scene counts or level offsets do not match.");
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, kWaitForever), "vkWaitForFences");
    Buffer readback;
    try {
        createBuffer(readback, meshBytes + clusterBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");
        VkMemoryBarrier before{};
        before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &before, 0, nullptr, 0, nullptr);
        const VkBufferCopy meshCopy{0, 0, meshBytes}, clusterCopy{0, meshBytes, clusterBytes};
        vkCmdCopyBuffer(commandBuffer_, meshBuffer_.handle, readback.handle, 1, &meshCopy);
        vkCmdCopyBuffer(commandBuffer_, clusterBuffer_.handle, readback.handle, 1, &clusterCopy);
        VkMemoryBarrier after{};
        after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        check(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        check(vkQueueWaitIdle(graphicsQueue_), "vkQueueWaitIdle");
        void* mapped = nullptr;
        check(vkMapMemory(device_, readback.memory, 0, meshBytes + clusterBytes, 0, &mapped), "vkMapMemory");
        const auto* bytes = static_cast<const char*>(mapped);
        const bool equal = std::memcmp(bytes, scene.mesh.vertices.data(), vertexBytes) == 0 &&
            std::memcmp(bytes + vertexBytes, packed.indices.data(), indexBytes) == 0 &&
            std::memcmp(bytes + meshBytes, packed.records.data(), clusterBytes) == 0;
        vkUnmapMemory(device_, readback.memory);
        if (!equal) throw std::runtime_error("GPU readback differs from the scene data.");
    } catch (...) {
        destroyBuffer(readback);
        throw;
    }
    destroyBuffer(readback);
    std::cout << "GPU readback passed: vertices, all indices, clusters, levels, nodes and children." << std::endl;
}

void VulkanRenderer::selectDepthFormat()
{
    depthFormat_ = VK_FORMAT_D32_SFLOAT; // same required capabilities as the directional shadow pass
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(gpu_, depthFormat_, &properties);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((properties.optimalTilingFeatures & needed) != needed)
        throw std::runtime_error("Sampleable D32 camera depth with readback is unavailable.");
}

void VulkanRenderer::createDepthResources()
{
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = depthFormat_;
    image.extent = {extent_.width, extent_.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_, &image, nullptr, &depthImage_), "vkCreateImage");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, depthImage_, &requirements);
    depthMemory_ = allocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkBindImageMemory(device_, depthImage_, depthMemory_, 0), "vkBindImageMemory");

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = depthImage_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = depthFormat_;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    check(vkCreateImageView(device_, &view, nullptr, &depthView_), "vkCreateImageView");
    const VkDescriptorImageInfo descriptor{VK_NULL_HANDLE, depthView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = cameraDepthSet_; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; write.pImageInfo = &descriptor;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanRenderer::recreateSwapchain(int width, int height)
{
    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
    destroySwapchain();

    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &capabilities),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    uint32_t formatCount = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &formatCount, nullptr),
          "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (formatCount == 0) {
        throw std::runtime_error("The window has no Vulkan surface formats.");
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &formatCount, formats.data()),
          "vkGetPhysicalDeviceSurfaceFormatsKHR");
    auto preferred = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& format) {
        return format.format == VK_FORMAT_B8G8R8A8_SRGB &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    const VkSurfaceFormatKHR format = preferred != formats.end() ? *preferred : formats.front();

    extent_ = capabilities.currentExtent;
    if (extent_.width == std::numeric_limits<uint32_t>::max()) {
        extent_.width = std::clamp(static_cast<uint32_t>(width),
                                   capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<uint32_t>(height),
                                    capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    const VkCompositeAlphaFlagBitsKHR alphaModes[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };
    const auto compositeAlpha = std::find_if(std::begin(alphaModes), std::end(alphaModes),
        [&](VkCompositeAlphaFlagBitsKHR mode) {
            return (capabilities.supportedCompositeAlpha & mode) != 0;
        });

    const uint32_t queueFamilies[] = {graphicsFamily_, presentFamily_};
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (graphicsFamily_ != presentFamily_) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = queueFamilies;
    }
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = *compositeAlpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR");

    if (format.format != colorFormat_) {
        shutdownUi();
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
        vkDestroyPipeline(device_, wireframePipeline_, nullptr);
        wireframePipeline_ = VK_NULL_HANDLE;
        vkDestroyPipeline(device_, boundsPipeline_, nullptr);
        boundsPipeline_ = VK_NULL_HANDLE;
        vkDestroyPipeline(device_, shadowDebugPipeline_, nullptr);
        shadowDebugPipeline_ = VK_NULL_HANDLE;
        vkDestroyPipeline(device_, cameraDebugPipeline_, nullptr);
        cameraDebugPipeline_ = VK_NULL_HANDLE;
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
        colorFormat_ = format.format;
        createRenderPass();
        createPipeline();
    }

    // Only one frame is in flight, so the framebuffers can share a depth image.
    createDepthResources();
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr), "vkGetSwapchainImagesKHR");
    std::vector<VkImage> images(imageCount);
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, images.data()), "vkGetSwapchainImagesKHR");
    initializeUi(capabilities.minImageCount, imageCount);
    swapchainImages_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        auto& image = swapchainImages_[i];
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colorFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        check(vkCreateImageView(device_, &viewInfo, nullptr, &image.view), "vkCreateImageView");

        const VkImageView attachments[] = {image.view, depthView_};
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = extent_.width;
        framebufferInfo.height = extent_.height;
        framebufferInfo.layers = 1;
        check(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &image.framebuffer), "vkCreateFramebuffer");

        // Present can still be using the semaphore after the frame fence signals.
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &image.renderFinished), "vkCreateSemaphore");
    }
}

void VulkanRenderer::createRenderPass()
{
    VkAttachmentDescription attachments[2]{};
    auto& color = attachments[0];
    color.format = colorFormat_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    auto& depth = attachments[1];
    depth.format = depthFormat_;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    const VkAttachmentReference colorAttachment{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthAttachment{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpasses[2]{};
    auto& subpass = subpasses[0];
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachment;
    subpass.pDepthStencilAttachment = &depthAttachment;
    const VkAttachmentReference depthInput{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    auto& overlay = subpasses[1];
    overlay.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    overlay.colorAttachmentCount = overlay.inputAttachmentCount = 1;
    overlay.pColorAttachments = &colorAttachment; overlay.pInputAttachments = &depthInput;

    // Wait for the acquired image before the render pass changes its layout.
    VkSubpassDependency dependencies[3]{};
    auto& dependency = dependencies[0];
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    const auto depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | depthStages;
    dependency.srcStageMask = dependency.dstStageMask | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                               VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1] = {0, 1, depthStages | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        depthStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_DEPENDENCY_BY_REGION_BIT};
    dependencies[2] = {1, VK_SUBPASS_EXTERNAL, depthStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0};

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 2;
    info.pSubpasses = subpasses;
    info.dependencyCount = 3;
    info.pDependencies = dependencies;
    check(vkCreateRenderPass(device_, &info, nullptr, &renderPass_), "vkCreateRenderPass");
}

void VulkanRenderer::createPipeline()
{
    VkShaderModule vertex = createShaderModule(device_, kVertexShader, sizeof(kVertexShader));
    VkShaderModule fragment = VK_NULL_HANDLE;
    try {
        fragment = createShaderModule(device_, kFragmentShader, sizeof(kFragmentShader));
    } catch (...) {
        vkDestroyShaderModule(device_, vertex, nullptr);
        throw;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;

    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)}
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo samples{};
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthTest{};
    depthTest.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthTest.depthTestEnable = VK_TRUE;
    depthTest.depthWriteEnable = VK_TRUE;
    depthTest.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState color{};
    color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &color;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depthTest;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout_;
    info.renderPass = renderPass_;
    const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    VkResult wireResult = VK_SUCCESS;
    if (result == VK_SUCCESS && wireframeSupported_) {
        rasterization.polygonMode = VK_POLYGON_MODE_LINE;
        wireResult = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &wireframePipeline_);
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    }

    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    check(result, "vkCreateGraphicsPipelines");
    check(wireResult, "vkCreateGraphicsPipelines (wireframe)");

    vertex = createShaderModule(device_, kBoundsVertexShader, sizeof(kBoundsVertexShader));
    fragment = VK_NULL_HANDLE;
    try {
        fragment = createShaderModule(device_, kBoundsFragmentShader, sizeof(kBoundsFragmentShader));
        stages[0].module = vertex;
        stages[1].module = fragment;
        vertexInput.vertexBindingDescriptionCount = 0;
        vertexInput.vertexAttributeDescriptionCount = 0;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        depthTest.depthWriteEnable = VK_FALSE;
        depthTest.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        rasterization.depthBiasEnable = VK_TRUE;
        rasterization.depthBiasConstantFactor = -1.0f;
        rasterization.depthBiasSlopeFactor = -1.0f;
        check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &boundsPipeline_),
              "vkCreateGraphicsPipelines(bounds)");
    } catch (...) {
        vkDestroyShaderModule(device_, fragment, nullptr);
        vkDestroyShaderModule(device_, vertex, nullptr);
        throw;
    }
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    const auto passPipeline = [&](const uint32_t* vs, size_t vsBytes, const uint32_t* fs, size_t fsBytes, VkPipeline& pipeline) {
        const auto v = createShaderModule(device_, vs, vsBytes);
        VkShaderModule f = VK_NULL_HANDLE;
        try {
            f = createShaderModule(device_, fs, fsBytes);
            stages[0].module = v; stages[1].module = f;
            check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines(pass)");
        } catch (...) { vkDestroyShaderModule(device_, f, nullptr); vkDestroyShaderModule(device_, v, nullptr); throw; }
        vkDestroyShaderModule(device_, f, nullptr); vkDestroyShaderModule(device_, v, nullptr);
    };
    if (!shadowPipeline_) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.vertexAttributeDescriptionCount = 1; // shadow vertex shader reads position only
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        depthTest.depthTestEnable = depthTest.depthWriteEnable = VK_TRUE;
        depthTest.depthCompareOp = VK_COMPARE_OP_LESS;
        rasterization.depthBiasEnable = VK_FALSE; // bias is applied in the receiver comparison
        rasterization.cullMode = VK_CULL_MODE_NONE; // two-sided thin displacement patches
        blend.attachmentCount = 0; info.renderPass = shadowRenderPass_;
        passPipeline(kShadowVertexShader, sizeof(kShadowVertexShader), kShadowFragmentShader, sizeof(kShadowFragmentShader), shadowPipeline_);
    }
    vertexInput.vertexBindingDescriptionCount = vertexInput.vertexAttributeDescriptionCount = 0;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    depthTest.depthTestEnable = depthTest.depthWriteEnable = VK_FALSE;
    rasterization.depthBiasEnable = VK_FALSE;
    blend.attachmentCount = 1; info.renderPass = renderPass_;
    passPipeline(kShadowDebugVertexShader, sizeof(kShadowDebugVertexShader), kShadowDebugFragmentShader, sizeof(kShadowDebugFragmentShader), shadowDebugPipeline_);
    info.subpass = 1;
    passPipeline(kShadowDebugVertexShader, sizeof(kShadowDebugVertexShader), kCameraDepthFragmentShader, sizeof(kCameraDepthFragmentShader), cameraDebugPipeline_);
}

void VulkanRenderer::destroySwapchain()
{
    cameraDepth_ = {};
    for (const auto& image : swapchainImages_) {
        vkDestroyFramebuffer(device_, image.framebuffer, nullptr);
        vkDestroyImageView(device_, image.view, nullptr);
        vkDestroySemaphore(device_, image.renderFinished, nullptr);
    }
    swapchainImages_.clear();
    vkDestroyImageView(device_, depthView_, nullptr);
    vkDestroyImage(device_, depthImage_, nullptr);
    vkFreeMemory(device_, depthMemory_, nullptr);
    depthView_ = VK_NULL_HANDLE;
    depthImage_ = VK_NULL_HANDLE;
    depthMemory_ = VK_NULL_HANDLE;
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanRenderer::initializeUi(uint32_t minImageCount, uint32_t imageCount)
{
    minImageCount = std::max(2u, minImageCount);
    if (imageCount < minImageCount) {
        throw std::runtime_error("ImGui requires at least two swapchain images.");
    }
    if (uiImageCount_ == imageCount && uiMinImageCount_ == minImageCount) {
        return;
    }
    shutdownUi();

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = kVulkanVersion;
    info.Instance = instance_;
    info.PhysicalDevice = gpu_;
    info.Device = device_;
    info.QueueFamily = graphicsFamily_;
    info.Queue = graphicsQueue_;
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE;
    info.MinImageCount = minImageCount;
    info.ImageCount = imageCount;
    info.PipelineInfoMain.RenderPass = renderPass_;
    info.PipelineInfoMain.Subpass = 1;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = [](VkResult result) { check(result, "ImGui Vulkan backend"); };
    if (!ImGui_ImplVulkan_Init(&info)) {
        throw std::runtime_error("Could not initialize the ImGui Vulkan backend.");
    }
    uiImageCount_ = imageCount;
    uiMinImageCount_ = minImageCount;
}

void VulkanRenderer::shutdownUi()
{
    if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData) {
        ImGui_ImplVulkan_Shutdown();
    }
    uiImageCount_ = 0;
    uiMinImageCount_ = 0;
}

void VulkanRenderer::recordDraw(uint32_t imageIndex, const OrbitCamera& camera,
    bool clusterColors, bool showBounds, int boundsCluster, bool computeGeometry, bool wireframe, ImDrawData* ui, bool measure)
{
    SourceDrawStats stats;
    stats.usesGeneratedGeometry = computeGeometry;
    stats.wireframe = wireframe;
    stats.display = renderSettings_.display;
    stats.shadowPass = renderSettings_.shadows || stats.display == RenderDisplay::ShadowMap;
    if (wireframe && !wireframeSupported_) throw std::runtime_error("Wireframe is not supported by this GPU.");
    if (computeGeometry && !compute_->matches(sceneGpu_, selection_, edgeSegments_, displacement_))
        throw std::runtime_error("Cannot draw stale or missing compute geometry.");
    stats.boundsPadding = computeGeometry ? compute_->output().maxDisplacement : 0;
    const float aspect = float(extent_.width) / float(extent_.height);
    const auto viewProjection = camera.viewProjection(aspect);
    if (stats.shadowPass) {
        stats.shadowCandidates = static_cast<uint32_t>(selection_.ranges.size());
        stats.shadowCasters = static_cast<uint32_t>(shadowSelection().ranges.size());
    }
    const LightingUniform lighting{lightMatrix_, glm::vec4(glm::normalize(renderSettings_.lightDirection), 0)};
    void* mapped = nullptr;
    check(vkMapMemory(device_, lightingBuffer_.memory, 0, sizeof(lighting), 0, &mapped), "vkMapMemory(lighting)");
    std::memcpy(mapped, &lighting, sizeof(lighting)); vkUnmapMemory(device_, lightingBuffer_.memory);
    std::vector<bool> visible(drawClusters_.size(), false);
    for (const auto& range : selection_.ranges) {
        const auto id = range.sourceClusterId;
        visible[id] = clusterVisible(drawClusters_[id], stats.boundsPadding, viewProjection, camera.eyePosition(aspect), renderSettings_);
        if (visible[id]) ++stats.visibleClusters;
    }
    const bool filtered = renderSettings_.frustumPreview || renderSettings_.maxDistance > 0;
    DrawPush push;
    push.boundsPadding = stats.boundsPadding;
    push.displayMode = static_cast<uint32_t>(stats.display);
    push.shadows = renderSettings_.shadows ? 1u : 0u;
    push.shadowBias = renderSettings_.shadowBias;
    check(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");
    if (measure && renderQueries_) {
        vkCmdResetQueryPool(commandBuffer_, renderQueries_, 0, 3);
        vkCmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, renderQueries_, 0);
    }
    const auto setViewport = [&](uint32_t width, uint32_t height) {
        const VkViewport viewport{0, 0, float(width), float(height), 0, 1};
        const VkRect2D scissor{{0, 0}, {width, height}};
        vkCmdSetViewport(commandBuffer_, 0, 1, &viewport); vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);
    };
    // Each view consumes its own selected ranges and draw commands.
    const auto drawMesh = [&](const auto& geometry, const auto& ranges, bool contiguous, bool shadow) {
        if (ranges.empty()) return;
        vkCmdBindVertexBuffers(commandBuffer_, 0, 1, &geometry.vertexBuffer, &geometry.vertexOffset);
        vkCmdBindIndexBuffer(commandBuffer_, geometry.indexBuffer, geometry.indexOffset, VK_INDEX_TYPE_UINT32);
        auto& calls = shadow ? stats.shadowDrawCalls : stats.meshDrawCalls;
        auto& triangles = shadow ? stats.shadowTriangles : stats.triangles;
        const bool sharedState = shadow || (!clusterColors && !filtered);
        const auto& source = shadow ? shadowScene() : sceneGpu_;
        const bool gpuCommands = source.indirectCommands && !computeGeometry;
        const auto indirect = [&](size_t slot, uint32_t count) {
            const auto offset = source.drawRecordOffset + slot * SceneGpuData::drawRecordStride + sizeof(uint32_t);
            vkCmdDrawIndexedIndirect(commandBuffer_, source.indirectCommands, offset, count,
                                     SceneGpuData::drawRecordStride);
        };
        if (gpuCommands && sharedState) {
            // The selector writes adjacent commands. Color/debug filtering below needs per-cluster state.
            for (size_t first = 0; first < ranges.size();) {
                const auto count = static_cast<uint32_t>(std::min<size_t>(ranges.size() - first, indirectDrawLimit_));
                indirect(first, count);
                first += count;
                ++calls;
            }
            triangles += (shadow ? shadowSelection() : selection_).triangleCount();
            return;
        }
        const auto draw = [&](uint32_t first, uint32_t count, uint32_t id, size_t slot) {
            if (!shadow) {
                const auto global = sceneGpu_.firstCluster + id;
                push.baseColor = clusterColors ? clusterColor(global) : kShadedColor;
                vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   64, sizeof(glm::vec4), &push.baseColor);
            }
            if (gpuCommands)
                indirect(slot, 1);
            else
                vkCmdDrawIndexed(commandBuffer_, count, 1, first, 0, 0);
            ++calls;
            triangles += count / 3;
        };
        if (contiguous && sharedState)
            draw(0, geometry.indexCount, 0, 0);
        else for (size_t slot = 0; slot < ranges.size(); ++slot) {
            const auto& range = ranges[slot];
            if (shadow || visible[range.sourceClusterId]) draw(range.firstIndex, range.indexCount, range.sourceClusterId, slot);
        }
    };
    const auto drawActiveMesh = [&](bool shadow) {
        if (computeGeometry) {
            const auto& output = shadow && separateShadowGeometry_ ? shadowCompute_->output() : compute_->output();
            drawMesh(output, output.ranges, true, shadow);
        }
        else {
            const auto& selected = shadow ? shadowSelection() : selection_;
            drawMesh(shadow ? shadowScene() : sceneGpu_, selected.ranges, selected.allClusters, shadow);
        }
    };
    if (stats.shadowPass) {
        VkClearValue clear{}; clear.depthStencil = {1, 0};
        VkRenderPassBeginInfo pass{}; pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = shadowRenderPass_; pass.framebuffer = shadowFramebuffer_;
        pass.renderArea.extent = {shadowMapSize, shadowMapSize}; pass.clearValueCount = 1; pass.pClearValues = &clear;
        vkCmdBeginRenderPass(commandBuffer_, &pass, VK_SUBPASS_CONTENTS_INLINE);
        setViewport(shadowMapSize, shadowMapSize);
        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        push.viewProjection = lightMatrix_;
        vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        drawActiveMesh(true);
        vkCmdEndRenderPass(commandBuffer_);
    }
    if (measure && renderQueries_) vkCmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, renderQueries_, 1);
    VkClearValue clear[2]{};
    clear[0].color = {{.025f, .034f, .05f, 1}}; clear[1].depthStencil = {1, 0};
    VkRenderPassBeginInfo pass{}; pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = renderPass_; pass.framebuffer = swapchainImages_[imageIndex].framebuffer;
    pass.renderArea.extent = extent_; pass.clearValueCount = 2; pass.pClearValues = clear;
    vkCmdBeginRenderPass(commandBuffer_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    setViewport(extent_.width, extent_.height);
    push.viewProjection = viewProjection;
    vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    const VkDescriptorSet sets[] = {sceneGpu_.clusterSet, shadowSet_};
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);
    if (stats.display == RenderDisplay::ShadowMap) {
        // Keep the 1:1 aspect of the square depth map when the window is wide.
        const float side = float(std::min(extent_.width, extent_.height));
        const VkViewport mapViewport{(float(extent_.width) - side) * .5f, (float(extent_.height) - side) * .5f, side, side, 0, 1};
        vkCmdSetViewport(commandBuffer_, 0, 1, &mapViewport);
        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowDebugPipeline_);
        vkCmdDraw(commandBuffer_, 3, 1, 0, 0);
    } else {
        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, wireframe ? wireframePipeline_ : pipeline_);
        drawActiveMesh(false);
        if (showBounds) {
            vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, boundsPipeline_);
            for (const auto& range : selection_.ranges) {
                if (!visible[range.sourceClusterId] || (boundsCluster >= 0 && range.sourceClusterId != uint32_t(boundsCluster))) continue;
                vkCmdDraw(commandBuffer_, 24, 1, 0, sceneGpu_.firstCluster + range.sourceClusterId); ++stats.boundsInstances;
            }
        }
    }
    vkCmdNextSubpass(commandBuffer_, VK_SUBPASS_CONTENTS_INLINE);
    if (stats.display == RenderDisplay::Depth) {
        setViewport(extent_.width, extent_.height);
        vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &cameraDepthSet_, 0, nullptr);
        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, cameraDebugPipeline_);
        vkCmdDraw(commandBuffer_, 3, 1, 0, 0);
    }
    if (ui) ImGui_ImplVulkan_RenderDrawData(ui, commandBuffer_);
    vkCmdEndRenderPass(commandBuffer_);
    if (measure && renderQueries_) vkCmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, renderQueries_, 2);
    check(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");
    lastDraw_ = stats;
}

bool VulkanRenderer::drawFrame(const OrbitCamera& camera, bool clusterColors, bool showBounds,
                               int boundsCluster, bool computeGeometry, bool wireframe, ImDrawData* ui, bool measure)
{
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, kWaitForever), "vkWaitForFences");
    if (renderMeasurementPending_) throw std::runtime_error("Consume the previous render measurement before submitting another frame.");

    // Generated measurements already forced the selector in ensureGeneratedGeometry().
    // Both routes also support callers that changed only the light since the last frame.
    prepareShadowPass(computeGeometry, measure && !computeGeometry);
    uint32_t imageIndex = 0;
    const VkResult acquire = vkAcquireNextImageKHR(
        device_, swapchain_, kAcquireTimeoutNs, imageAvailable_, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        rebuildSwapchain_ = true;
        return false;
    }
    if (acquire == VK_TIMEOUT) {
        return false;
    }
    if (acquire != VK_SUBOPTIMAL_KHR) {
        check(acquire, "vkAcquireNextImageKHR");
    }

    recordDraw(imageIndex, camera, clusterColors, showBounds, boundsCluster, computeGeometry, wireframe, ui, measure);
    // Reset only when a submission will signal the fence again.
    check(vkResetFences(device_, 1, &frameFence_), "vkResetFences");
    const VkSemaphore renderFinished = swapchainImages_[imageIndex].renderFinished;
    // A measured interval starts only after the swapchain image is available.
    // Ordinary frames retain the original overlap; measurement serializes at TOP.
    const VkPipelineStageFlags waitStage = measure ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished;
    check(vkQueueSubmit(graphicsQueue_, 1, &submit, frameFence_), "vkQueueSubmit");
    ++frameSerial_;
    cameraDepth_ = {};
    if (renderSettings_.display != RenderDisplay::ShadowMap) {
        cameraDepth_ = {depthView_, shadowSampler_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, extent_,
            camera.viewProjection(float(extent_.width) / extent_.height), frameSerial_, sceneGpu_.sceneRevision, !wireframe};
    }
    renderMeasurementPending_ = measure && renderQueries_;

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    const VkResult result = vkQueuePresentKHR(presentQueue_, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        rebuildSwapchain_ = true;
        return true;
    }
    check(result, "vkQueuePresentKHR");
    rebuildSwapchain_ = acquire == VK_SUBOPTIMAL_KHR;
    return true;
}

std::optional<double> VulkanRenderer::finishRenderMeasurement()
{
    if (!renderQueries_) return std::nullopt;
    if (!renderMeasurementPending_) throw std::runtime_error("No submitted render measurement to read.");
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, kWaitForever), "vkWaitForFences(render measurement)");
    uint64_t timestamps[3]{};
    check(vkGetQueryPoolResults(device_, renderQueries_, 0, 3, sizeof(timestamps), timestamps,
                               sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults(render)");
    const uint64_t mask = timestampValidBits_ == 64 ? ~uint64_t{0} : (uint64_t{1} << timestampValidBits_) - 1;
    renderMeasurementPending_ = false;
    const auto elapsed = [&](int start, int end) { return double((timestamps[end] - timestamps[start]) & mask) * double(timestampPeriod_) / 1'000'000.0; };
    mainMs_ = elapsed(1, 2);
    shadowMs_ = lastDraw_.shadowPass ? std::optional<double>(elapsed(0, 1)) : std::nullopt;
    return elapsed(0, 2);
}
