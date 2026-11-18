#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "MeshSource.h"
#include "SceneData.h"
#include "SceneDiagnostics.h"
#include "Viewer.h"
#include "GeometryGenerationPlan.h"

#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

TessellationFixture parseFixtureName(const std::filesystem::path& name)
{
    if (name == "triangle") return TessellationFixture::Triangle;
    if (name == "plane") return TessellationFixture::Plane;
    if (name == "shadow") return TessellationFixture::ShadowStage;
    if (name == "shadow-casters") return TessellationFixture::ShadowCasters;
    if (name == "grid") return TessellationFixture::ClusterGrid;
    throw std::runtime_error("Unknown fixture; choose triangle, plane, grid, shadow or shadow-casters.");
}

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
                     "       [--fixture triangle|plane|grid|shadow|shadow-casters]\n"
                     "       [--cluster-colors] [--bounds] [--compute-copy] [--verify-gpu] [--smoke-test]\n"
                     "       [--subdivision 1|2|4|8] [--wireframe]\n"
                     "       [--geometry-level N] [--hide-ui] [--view-config file.acrview] [--save-view file.acrview]\n"
                     "       [--shadows] [--shadow-bias 0..0.08] [--depth-view | --shadow-map]\n"
                     "       [--shadow-cull all|frustum] [--shadow-coverage 0.05..1] [--light-direction X Y Z] [--shadow-cull-smoke]\n"
                     "       [--frustum-preview] [--max-distance model_units] [--render-smoke]\n"
                     "       [--displacement amplitude (0..1000)] [--heightmap file.pgm]\n"
                     "       [--selection all|first-last|empty|last-first]\n"
                     "       [--benchmark output.json [--benchmark-warmup 10] [--benchmark-samples 50]]\n"
                     "       --smoke-roundtrip output.acr (hidden save/mock/reload test)\n"
                     "       --hierarchy-roundtrip output.acr (verify all levels on the GPU)\n"
                     "       --preprocess input.obj output.acr\n"
                     "       --make-mock output.acr\n"
                     "       --validate model.acr\n"
                     "       --check-environment\n";
    };
    const bool checkEnvironment = argc == 2 && argument(1) == "--check-environment";
    bool useCube = false, useMock = false;
    std::optional<TessellationFixture> fixture;
    std::filesystem::path inputPath;
    ViewerOptions options;
    bool benchmarkCountsSpecified = false;
    options.reportDirectory = std::filesystem::absolute(argument(0)).parent_path() / "benchmarks";
    try {
        if (argc == 2 && argument(1) == "--help") { usage(); return 0; }
        if (argc == 4 && argument(1) == "--preprocess") {
            const auto loaded = loadSceneWithStats(argument(2));
            printSceneSummary(std::cout, loaded);
            const auto saved = saveSceneVerified(argument(3), loaded.scene);
            std::cout << "Saved + verified ACR v" << sceneFileVersion(loaded.scene) << ": " << saved.path.u8string() << " | " << saved.fileBytes
                      << " bytes | CPU save (ms): " << saved.cpuSaveMs << " | CPU reload (ms): " << saved.cpuReloadMs << '\n';
            return 0;
        }
        if (argc == 3 && argument(1) == "--make-mock") {
            const auto loaded = makeMockSceneWithStats();
            printSceneSummary(std::cout, loaded);
            const auto saved = saveSceneVerified(argument(2), loaded.scene);
            std::cout << "Saved + verified fixed 4-cluster mock scene: " << saved.fileBytes << " bytes.\n";
            return 0;
        }
        if (argc == 3 && argument(1) == "--validate") {
            const auto loaded = loadSceneWithStats(argument(2));
            printSceneSummary(std::cout, loaded);
            std::cout << "Scene validation passed.\n";
            return 0;
        }
        if (!checkEnvironment) {
            // Restore a saved baseline first. Explicit CLI settings override it.
            for (int i = 1; i < argc; ++i)
                if (argument(i) == "--view-config") {
                    if (++i >= argc || options.initialView) throw std::runtime_error("Choose one .acrview file.");
                    options.initialView = loadViewConfig(argument(i));
                    const auto& v = *options.initialView;
                    options.renderSettings = v.render;
                    options.geometryLevel = v.level;
                    options.hidePanel = v.hidePanel;
                    options.clusterColors = v.clusterColors;
                    options.showBounds = v.bounds;
                    options.computeGeometry = v.generated;
                    options.wireframe = v.wire;
                    options.displacement = v.displacement;
                    options.heightMapPath = v.heightPath;
                    options.edgeSegments = v.segments;
                    options.selectionPreset = v.selection;
                    options.benchmarkOptions = v.sampling;
                }
            for (int i = 1; i < argc; ++i) {
                const auto option = argument(i);
                if (option == "--cluster-colors") options.clusterColors = true;
                else if (option == "--hide-ui") options.hidePanel = true;
                else if (option == "--shadow-cull-smoke")
                    options.shadowCullSmoke = true;
                else if (option == "--view-config")
                    ++i; // Loaded before applying CLI overrides.
                else if (option == "--save-view") {
                    if (++i >= argc) throw std::runtime_error("Missing .acrview path.");
                    options.saveConfigPath = argument(i);
                }
                else if (option == "--geometry-level") {
                    if (++i >= argc) throw std::runtime_error("Missing geometry level.");
                    const auto value = argument(i).u8string();
                    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                        throw std::runtime_error("Geometry level must be a nonnegative integer.");
                    const auto parsed = std::stoull(value);
                    if (parsed > INT32_MAX) throw std::runtime_error("Geometry level is out of range.");
                    options.geometryLevel = static_cast<uint32_t>(parsed);
                }
                else if (option == "--bounds") options.showBounds = true;
                else if (option == "--verify-gpu") options.verifyGpu = true;
                else if (option == "--compute-copy") options.computeGeometry = true;
                else if (option == "--wireframe") options.wireframe = true;
                else if (option == "--shadows") options.renderSettings.shadows = true;
                else if (option == "--shadow-cull") {
                    if (++i >= argc || (argument(i) != "frustum" && argument(i) != "all"))
                        throw std::runtime_error("--shadow-cull requires all or frustum.");
                    options.renderSettings.shadowCulling = argument(i) == "frustum";
                }
                else if (option == "--light-direction") {
                    for (int axis = 0; axis < 3; ++axis) {
                        if (++i >= argc) throw std::runtime_error("--light-direction requires three components.");
                        const auto value = argument(i).u8string();
                        size_t consumed = 0;
                        options.renderSettings.lightDirection[axis] = std::stof(value, &consumed);
                        if (consumed != value.size()) throw std::runtime_error("Invalid light direction component.");
                    }
                }
                else if (option == "--depth-view") options.renderSettings.display = RenderDisplay::Depth;
                else if (option == "--shadow-map") options.renderSettings.display = RenderDisplay::ShadowMap;
                else if (option == "--frustum-preview") options.renderSettings.frustumPreview = true;
                else if (option == "--render-smoke") options.renderSmoke = true;
                else if (option == "--shadow-bias" || option == "--shadow-coverage" || option == "--max-distance") {
                    if (++i >= argc) throw std::runtime_error("Missing render setting value.");
                    const auto value = argument(i).u8string(); size_t consumed = 0;
                    const float number = std::stof(value, &consumed);
                    if (consumed != value.size()) throw std::runtime_error("Invalid render setting value.");
                    if (option == "--shadow-bias") options.renderSettings.shadowBias = number;
                    else if (option == "--shadow-coverage") options.renderSettings.shadowCoverage = number;
                    else options.renderSettings.maxDistance = number;
                }
                else if (option == "--benchmark") {
                    if (++i >= argc) throw std::runtime_error("--benchmark requires an output .json path.");
                    options.benchmarkPath = argument(i);
                    if (options.benchmarkPath.extension() != ".json") throw std::runtime_error("Benchmark output must use .json.");
                }
                else if (option == "--benchmark-warmup" || option == "--benchmark-samples") {
                    if (++i >= argc) throw std::runtime_error("Benchmark count is missing.");
                    const auto value = argument(i).u8string();
                    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                        throw std::runtime_error("Benchmark counts must be positive integers.");
                    const auto count = std::stoul(value);
                    if (count > 10000) throw std::runtime_error("Benchmark count exceeds 10000.");
                    if (option == "--benchmark-warmup") options.benchmarkOptions.warmup = static_cast<uint32_t>(count);
                    else options.benchmarkOptions.samples = static_cast<uint32_t>(count);
                    benchmarkCountsSpecified = true;
                }
                else if (option == "--selection") {
                    if (++i >= argc) throw std::runtime_error("--selection requires all, first-last, empty, or last-first.");
                    const auto value = argument(i);
                    if (value == "all") options.selectionPreset = 0;
                    else if (value == "first-last") options.selectionPreset = 1;
                    else if (value == "empty") options.selectionPreset = 2;
                    else if (value == "last-first") options.selectionPreset = 3;
                    else throw std::runtime_error("Unknown cluster selection preset.");
                }
                else if (option == "--displacement") {
                    if (++i >= argc) throw std::runtime_error("--displacement requires an amplitude in model units.");
                    const auto value = argument(i).u8string();
                    size_t consumed = 0;
                    options.displacement = {true, std::stof(value, &consumed)};
                    validateDisplacement(options.displacement);
                    if (consumed != value.size()) throw std::runtime_error("Invalid displacement amplitude.");
                    options.computeGeometry = true;
                }
                else if (option == "--heightmap") {
                    if (++i >= argc) throw std::runtime_error("--heightmap requires a PGM file path.");
                    options.heightMapPath = argument(i);
                }
                else if (option == "--subdivision") {
                    if (++i >= argc) throw std::runtime_error("--subdivision requires 1, 2, 4 or 8.");
                    const auto value = argument(i).u8string();
                    if (value.size() != 1 || value[0] < '0' || value[0] > '9' ||
                        !validEdgeSegments(static_cast<uint32_t>(value[0] - '0'))) {
                        throw std::runtime_error("--subdivision requires 1, 2, 4 or 8.");
                    }
                    options.edgeSegments = static_cast<uint32_t>(value[0] - '0');
                    options.computeGeometry = true;
                }
                else if (option == "--smoke-test") options.smokeTest = true;
                else if (option == "--hierarchy-roundtrip") {
                    if (++i >= argc) throw std::runtime_error("Missing hierarchy roundtrip output path.");
                    options.hierarchyRoundtripPath = argument(i);
                }
                else if (option == "--smoke-roundtrip") {
                    if (++i >= argc) throw std::runtime_error("--smoke-roundtrip requires an output ACR path.");
                    options.smokeTest = true;
                    options.smokeRoundtripPath = argument(i);
                }
                else {
                    if (useCube || useMock || fixture || !inputPath.empty()) throw std::runtime_error("Choose only one scene source.");
                    if (option == "--cube") useCube = true;
                    else if (option == "--mock") useMock = true;
                    else if (option == "--fixture") {
                        if (++i >= argc) throw std::runtime_error("--fixture requires a scene name.");
                        fixture = parseFixtureName(argument(i));
                    }
                    else if (!option.empty() && option.u8string().front() != '-') inputPath = option;
                    else throw std::runtime_error("Unknown option: " + option.u8string());
                }
            }
            validateRenderSettings(options.renderSettings);
            if (options.shadowCullSmoke && (options.smokeTest || options.renderSmoke || !options.hierarchyRoundtripPath.empty() ||
                !options.benchmarkPath.empty() || !options.saveConfigPath.empty()))
                throw std::runtime_error("Run the shadow caster check separately from other tests.");
            if (!options.saveConfigPath.empty() && (options.smokeTest || options.renderSmoke || !options.hierarchyRoundtripPath.empty()))
                throw std::runtime_error("Save a view separately from the acceptance checks.");
            if (!options.hierarchyRoundtripPath.empty() && (options.renderSmoke || options.smokeTest || !options.benchmarkPath.empty()))
                throw std::runtime_error("Run the hierarchy roundtrip separately from other test modes.");
            validateBenchmarkOptions(options.benchmarkOptions);
            if (benchmarkCountsSpecified && options.benchmarkPath.empty()) throw std::runtime_error("Benchmark counts require --benchmark.");
            if (!options.benchmarkPath.empty() && options.smokeTest) throw std::runtime_error("Choose either benchmark or smoke test.");
            if (options.renderSmoke && (options.smokeTest || !options.benchmarkPath.empty())) throw std::runtime_error("Render integration checks run separately from smoke/benchmark options.");
            if (options.smokeTest && (options.renderSettings.display != RenderDisplay::Shaded || options.renderSettings.frustumPreview || options.renderSettings.maxDistance > 0))
                throw std::runtime_error("Use --render-smoke for display and visibility integration checks.");
            if (options.smokeTest && options.selectionPreset != 0) throw std::runtime_error("Smoke tests manage their own selection.");
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
            if (fixture) {
                exitCode = runViewer(makeFixtureSceneWithStats(*fixture), options);
            } else if (useCube) {
                exitCode = runViewer(makeCubeSceneWithStats(), options);
            } else if (useMock) {
                exitCode = runViewer(makeMockSceneWithStats(), options);
            } else if (inputPath.empty() && options.initialView) {
                exitCode = runViewer(loadViewScene(*options.initialView),options);
            } else {
                const auto path = !inputPath.empty() ? inputPath :
                    std::filesystem::absolute(argv[0]).parent_path() / "assets" / "suzanne.obj";
                exitCode = runViewer(loadSceneWithStats(path), options);
            }
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            exitCode = 1;
        }
    }

    glfwTerminate();
    return exitCode;
}
