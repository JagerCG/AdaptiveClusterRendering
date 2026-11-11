#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "MeshSource.h"
#include "SceneData.h"
#include "Viewer.h"

#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
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
    const auto argument = [&](int i) { return std::filesystem::path(argv[i]); };
    const auto usage = [] {
        std::cout << "Usage: AdaptiveClusterRendering [model.obj | model.acr | --cube | --mock]\n"
                     "       [--cluster-colors] [--bounds] [--verify-gpu] [--smoke-test]\n"
                     "       --preprocess input.obj output.acr\n"
                     "       --make-mock output.acr\n"
                     "       --validate model.acr\n"
                     "       --check-environment\n";
    };
    const bool checkEnvironment = argc == 2 && argument(1) == "--check-environment";
    bool useCube = false, useMock = false;
    std::filesystem::path inputPath;
    ViewerOptions options;
    try {
        if (argc == 2 && argument(1) == "--help") { usage(); return 0; }
        if (argc == 4 && argument(1) == "--preprocess") {
            const auto scene = loadScene(argument(2));
            saveScene(argument(3), scene);
            std::cout << "Saved " << scene.dataset.clusters.size() << " clusters to " << argument(3).u8string() << '\n';
            return 0;
        }
        if (argc == 3 && argument(1) == "--make-mock") {
            saveScene(argument(2), makeMockScene());
            std::cout << "Saved fixed 4-cluster mock scene.\n";
            return 0;
        }
        if (argc == 3 && argument(1) == "--validate") {
            const auto scene = loadScene(argument(2));
            std::cout << "Scene validation passed: " << scene.mesh.vertices.size() << " vertices, "
                      << scene.dataset.indices.size() / 3 << " triangles, " << scene.dataset.clusters.size()
                      << " clusters.\n";
            return 0;
        }
        if (!checkEnvironment) {
            for (int i = 1; i < argc; ++i) {
                const auto option = argument(i);
                if (option == "--cluster-colors") options.clusterColors = true;
                else if (option == "--bounds") options.showBounds = true;
                else if (option == "--verify-gpu") options.verifyGpu = true;
                else if (option == "--smoke-test") options.smokeTest = true;
                else {
                    if (useCube || useMock || !inputPath.empty()) throw std::runtime_error("Choose only one scene source.");
                    if (option == "--cube") useCube = true;
                    else if (option == "--mock") useMock = true;
                    else if (!option.empty() && option.u8string().front() != '-') inputPath = option;
                    else throw std::runtime_error("Unknown option: " + option.u8string());
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        usage();
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
                exitCode = runViewer(buildScene(makeCube()), "Cube", options);
            } else if (useMock) {
                exitCode = runViewer(makeMockScene(), "Mock (4 clusters)", options);
            } else {
                const auto path = !inputPath.empty() ? inputPath :
                    std::filesystem::absolute(argv[0]).parent_path() / "assets" / "suzanne.obj";
                exitCode = runViewer(loadScene(path), path.filename().u8string(), options);
            }
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            exitCode = 1;
        }
    }

    glfwTerminate();
    return exitCode;
}
