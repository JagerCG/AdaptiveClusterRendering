#include "VulkanRenderer.h"

#include "ClusterBuilder.h"
#include "MeshSource.h"
#include "OrbitCamera.h"

#include <GLFW/glfw3.h>
#include <glm/common.hpp>
#include <glm/vec4.hpp>
#include <imgui_impl_vulkan.h>

#include <algorithm>
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
        shutdownUi();
        destroySwapchain();
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        vkDestroyFence(device_, frameFence_, nullptr);
        vkDestroySemaphore(device_, imageAvailable_, nullptr);
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        destroyBuffer(meshBuffer_);
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
            const bool canDraw = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
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
        }
    }
    if (gpu_ == VK_NULL_HANDLE) {
        throw std::runtime_error("No Vulkan 1.2 device can draw to this window.");
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
    check(vkCreateDevice(gpu_, &info, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
    std::cout << "Rendering on " << selectedProperties.deviceName << std::endl;
}

void VulkanRenderer::createFrameResources()
{
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

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const VkPushConstantRange ranges[] = {
        {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)},
        {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(glm::mat4), sizeof(glm::vec4)}
    };
    layoutInfo.pushConstantRangeCount = 2;
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
    check(vkBindBufferMemory(device_, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");
}

void VulkanRenderer::destroyBuffer(const Buffer& buffer)
{
    vkDestroyBuffer(device_, buffer.handle, nullptr);
    vkFreeMemory(device_, buffer.memory, nullptr);
}

void VulkanRenderer::uploadMesh(const MeshSource& mesh, const std::vector<uint32_t>& indices)
{
    if (mesh.vertices.empty() || indices.empty() || indices.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Mesh must contain vertices and a drawable index list.");
    }

    // The index list follows the vertices in the same buffer.
    const size_t vertexBytes = mesh.vertices.size() * sizeof(Vertex);
    const size_t indexBytes = indices.size() * sizeof(uint32_t);
    const VkDeviceSize bufferSize = vertexBytes + indexBytes;
    const auto meshUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, kWaitForever), "vkWaitForFences");
    Buffer replacement;
    Buffer staging;
    try {
        createBuffer(replacement, bufferSize, meshUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const auto stagingMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        createBuffer(staging, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stagingMemory);
        void* mapped = nullptr;
        check(vkMapMemory(device_, staging.memory, 0, bufferSize, 0, &mapped), "vkMapMemory");
        auto* bytes = static_cast<char*>(mapped);
        std::memcpy(bytes, mesh.vertices.data(), vertexBytes);
        std::memcpy(bytes + vertexBytes, indices.data(), indexBytes);
        vkUnmapMemory(device_, staging.memory);

        check(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");
        const VkBufferCopy copy{0, 0, bufferSize};
        vkCmdCopyBuffer(commandBuffer_, staging.handle, replacement.handle, 1, &copy);

        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = replacement.handle;
        barrier.size = bufferSize;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
        check(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        check(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        check(vkQueueWaitIdle(graphicsQueue_), "vkQueueWaitIdle");
    } catch (...) {
        destroyBuffer(staging);
        destroyBuffer(replacement);
        throw;
    }
    destroyBuffer(staging);
    destroyBuffer(meshBuffer_);
    meshBuffer_ = replacement;
    indexOffset_ = vertexBytes;
    indexCount_ = static_cast<uint32_t>(indices.size());
}

void VulkanRenderer::selectDepthFormat()
{
    const VkFormat formats[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
    for (VkFormat format : formats) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(gpu_, format, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depthFormat_ = format;
            return;
        }
    }
    throw std::runtime_error("The device has no supported depth format.");
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
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
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
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    const VkAttachmentReference colorAttachment{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthAttachment{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachment;
    subpass.pDepthStencilAttachment = &depthAttachment;

    // Wait for the acquired image before the render pass changes its layout.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    const auto depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | depthStages;
    dependency.dstStageMask = dependency.srcStageMask;
    dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
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

    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    check(result, "vkCreateGraphicsPipelines");
}

void VulkanRenderer::destroySwapchain()
{
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

void VulkanRenderer::recordDraw(uint32_t imageIndex, const OrbitCamera& camera, const ClusterDataset& dataset,
                                bool clusterColors, ImDrawData* ui)
{
    check(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");

    VkClearValue clearValues[2]{};
    clearValues[0].color = {{0.025f, 0.034f, 0.05f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = renderPass_;
    pass.framebuffer = swapchainImages_[imageIndex].framebuffer;
    pass.renderArea.extent = extent_;
    pass.clearValueCount = 2;
    pass.pClearValues = clearValues;
    vkCmdBeginRenderPass(commandBuffer_, &pass, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent_.width),
                              static_cast<float>(extent_.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(commandBuffer_, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    const float aspectRatio = viewport.width / viewport.height;
    const glm::mat4 viewProjection = camera.viewProjection(aspectRatio);
    vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(viewProjection), &viewProjection);

    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(commandBuffer_, 0, 1, &meshBuffer_.handle, &vertexOffset);
    vkCmdBindIndexBuffer(commandBuffer_, meshBuffer_.handle, indexOffset_, VK_INDEX_TYPE_UINT32);
    if (clusterColors) {
        for (uint32_t id = 0; id < dataset.clusters.size(); ++id) {
            const Cluster& cluster = dataset.clusters[id];
            const glm::vec4 color = clusterColor(id);
            vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                               sizeof(glm::mat4), sizeof(color), &color);
            vkCmdDrawIndexed(commandBuffer_, cluster.indexCount, 1, cluster.firstIndex, 0, 0);
        }
    } else {
        vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           sizeof(glm::mat4), sizeof(kShadedColor), &kShadedColor);
        vkCmdDrawIndexed(commandBuffer_, indexCount_, 1, 0, 0, 0);
    }

    ImGui_ImplVulkan_RenderDrawData(ui, commandBuffer_);
    vkCmdEndRenderPass(commandBuffer_);
    check(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");
}

void VulkanRenderer::drawFrame(const OrbitCamera& camera, const ClusterDataset& dataset, bool clusterColors,
                               ImDrawData* ui)
{
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, kWaitForever), "vkWaitForFences");

    uint32_t imageIndex = 0;
    const VkResult acquire = vkAcquireNextImageKHR(
        device_, swapchain_, kAcquireTimeoutNs, imageAvailable_, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        rebuildSwapchain_ = true;
        return;
    }
    if (acquire == VK_TIMEOUT) {
        return;
    }
    if (acquire != VK_SUBOPTIMAL_KHR) {
        check(acquire, "vkAcquireNextImageKHR");
    }

    recordDraw(imageIndex, camera, dataset, clusterColors, ui);
    // Reset only when a submission will signal the fence again.
    check(vkResetFences(device_, 1, &frameFence_), "vkResetFences");
    const VkSemaphore renderFinished = swapchainImages_[imageIndex].renderFinished;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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
        return;
    }
    check(result, "vkQueuePresentKHR");
    rebuildSwapchain_ = acquire == VK_SUBOPTIMAL_KHR;
}
