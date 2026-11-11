#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "MeshSource.h"
#include "ObjLoader.h"
#include "Viewer.h"

#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <vector>

namespace {

void printGlfwError(int code, const char* description)
{
    std::cerr << "GLFW error " << code << ": " << description << '\n';
}

bool checkVulkanEnvironment()
{
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Adaptive Cluster Rendering";
    application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application.apiVersion = VK_API_VERSION_1_2;

    uint32_t extensionCount = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
    if (extensions == nullptr) {
        std::cerr << "Required Vulkan window extensions are unavailable.\n";
        return false;
    }

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &application;
    createInfo.enabledExtensionCount = extensionCount;
    createInfo.ppEnabledExtensionNames = extensions;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::cerr << "Cannot create Vulkan 1.2 instance. VkResult = " << result << '\n';
        return false;
    }

    uint32_t deviceCount = 0;
    std::vector<VkPhysicalDevice> devices;
    do {
        result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (result != VK_SUCCESS || deviceCount == 0) {
            break;
        }
        devices.resize(deviceCount);
        result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    } while (result == VK_INCOMPLETE);

    if (result != VK_SUCCESS) {
        std::cerr << "Cannot enumerate Vulkan devices. VkResult = " << result << '\n';
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    if (deviceCount == 0) {
        std::cerr << "No Vulkan physical devices found.\n";
        vkDestroyInstance(instance, nullptr);
        return false;
    }

    bool supportsVulkan12 = false;
    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        std::cout << "GPU: " << properties.deviceName << " | Vulkan "
                  << VK_API_VERSION_MAJOR(properties.apiVersion) << '.'
                  << VK_API_VERSION_MINOR(properties.apiVersion) << '.'
                  << VK_API_VERSION_PATCH(properties.apiVersion) << '\n';
        if (properties.apiVersion >= VK_API_VERSION_1_2) {
            supportsVulkan12 = true;
        }
    }

    uint32_t layerCount = 0;
    std::vector<VkLayerProperties> layers;
    do {
        result = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        if (result != VK_SUCCESS || layerCount == 0) {
            break;
        }
        layers.resize(layerCount);
        result = vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    } while (result == VK_INCOMPLETE);

    bool hasValidation = false;
    if (result == VK_SUCCESS) {
        for (uint32_t i = 0; i < layerCount; ++i) {
            if (std::strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                hasValidation = true;
                break;
            }
        }
    }
    std::cout << "Khronos validation layer: " << (hasValidation ? "available" : "unavailable") << '\n';

    vkDestroyInstance(instance, nullptr);
    if (!supportsVulkan12) {
        std::cerr << "A GPU supporting Vulkan 1.2 or newer is required.\n";
    }
    return supportsVulkan12;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    const std::filesystem::path argument = argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
    const bool checkEnvironment = argument == "--check-environment";
    const bool useCube = argument == "--cube";
    const bool loadFile = argc == 2 && !argument.empty() && argument.u8string().front() != '-';
    if (argc != 1 && !checkEnvironment && !useCube && !loadFile) {
        std::cerr << "Usage: AdaptiveClusterRendering [model.obj | --cube | --check-environment]\n";
        return 1;
    }

    glfwSetErrorCallback(printGlfwError);
    if (glfwInit() != GLFW_TRUE) {
        return 1;
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        std::cerr << "Vulkan loader or driver is unavailable.\n";
        glfwTerminate();
        return 1;
    }

    int exitCode = 0;
    if (checkEnvironment) {
        std::cout << "Adaptive Cluster Rendering - environment check\n";
        std::cout << "GLFW " << glfwGetVersionString() << '\n';
        if (checkVulkanEnvironment()) {
            std::cout << "Environment check passed.\n";
        } else {
            exitCode = 1;
        }
    } else {
        try {
            if (useCube) {
                exitCode = runViewer(makeCube(), "Cube");
            } else {
                const auto path = loadFile ? argument :
                    std::filesystem::absolute(argv[0]).parent_path() / "assets" / "suzanne.obj";
                exitCode = runViewer(loadObj(path), path.filename().u8string());
            }
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            exitCode = 1;
        }
    }

    glfwTerminate();
    return exitCode;
}
