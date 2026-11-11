#include "Viewer.h"

#include "ClusterBuilder.h"
#include "MeshSource.h"
#include "SceneData.h"
#include "OrbitCamera.h"
#include "VulkanRenderer.h"

#include <GLFW/glfw3.h>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#endif

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

struct ViewerInput {
    double cursorX = 0.0;
    double cursorY = 0.0;
    float dragX = 0.0f;
    float dragY = 0.0f;
    float scroll = 0.0f;
    bool dragging = false;
    bool resetCamera = false;
};

struct ViewerModel {
    SceneData scene;
    OrbitCamera camera;
    std::string name;
};

class ViewerUi {
public:
    explicit ViewerUi(GLFWwindow* window)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
#ifdef _WIN32
        wchar_t windowsDirectory[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            const auto fontPath = std::filesystem::path(windowsDirectory) / "Fonts" / "msyh.ttc";
            std::error_code error;
            if (std::filesystem::is_regular_file(fontPath, error)) {
                io.Fonts->AddFontDefault();
                ImFontConfig font{};
                font.MergeMode = true;
                io.Fonts->AddFontFromFileTTF(fontPath.u8string().c_str(), 13.0f, &font);
            }
        }
#endif
        ImGui::StyleColorsDark();
        // Readable on a presentation display; independent of model/camera scale.
        ImGui::GetStyle().FontScaleMain = 1.4f;
        ImGui::GetStyle().ScaleAllSizes(1.2f);
        if (!ImGui_ImplGlfw_InitForVulkan(window, true)) {
            ImGui::DestroyContext();
            throw std::runtime_error("Could not initialize ImGui input.");
        }
    }

    ~ViewerUi()
    {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    ViewerUi(const ViewerUi&) = delete;
    ViewerUi& operator=(const ViewerUi&) = delete;
};

OrbitCamera frameMesh(const MeshSource& mesh)
{
    glm::vec3 minimum = mesh.vertices.front().position;
    glm::vec3 maximum = minimum;
    for (const Vertex& vertex : mesh.vertices) {
        minimum = glm::min(minimum, vertex.position);
        maximum = glm::max(maximum, vertex.position);
    }

    const glm::vec3 halfSize = (maximum - minimum) * 0.5f;
    const float radius = glm::length(halfSize);
    if (radius <= 0.0f || !std::isfinite(radius)) {
        throw std::runtime_error("Cannot frame a mesh with invalid bounds.");
    }
    return OrbitCamera(minimum + halfSize, radius);
}

ViewerModel makeModel(SceneData scene, std::string name)
{
    validateScene(scene);
    auto camera = frameMesh(scene.mesh);
    return {std::move(scene), camera, std::move(name)};
}

void handleMouseButton(GLFWwindow* window, int button, int action, int)
{
    if (button != GLFW_MOUSE_BUTTON_LEFT) {
        return;
    }
    auto& input = *static_cast<ViewerInput*>(glfwGetWindowUserPointer(window));
    input.dragging = action == GLFW_PRESS;
    if (input.dragging) {
        glfwGetCursorPos(window, &input.cursorX, &input.cursorY);
    }
}

void handleCursor(GLFWwindow* window, double x, double y)
{
    auto& input = *static_cast<ViewerInput*>(glfwGetWindowUserPointer(window));
    if (input.dragging) {
        input.dragX += static_cast<float>(x - input.cursorX);
        input.dragY += static_cast<float>(y - input.cursorY);
    }
    input.cursorX = x;
    input.cursorY = y;
}

void handleScroll(GLFWwindow* window, double, double scrollY)
{
    auto& input = *static_cast<ViewerInput*>(glfwGetWindowUserPointer(window));
    input.scroll += static_cast<float>(scrollY);
}

void handleKey(GLFWwindow* window, int key, int, int action, int)
{
    if (action != GLFW_PRESS) {
        return;
    }
    if (key == GLFW_KEY_ESCAPE) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    } else if (key == GLFW_KEY_R) {
        auto& input = *static_cast<ViewerInput*>(glfwGetWindowUserPointer(window));
        input.resetCamera = true;
    }
}

void updateCamera(ViewerInput& input, OrbitCamera& camera)
{
    // Decide who gets the mouse after ImGui has processed this frame's events.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) {
        input.dragging = false;
    } else {
        camera.orbit(input.dragX, input.dragY);
        camera.zoom(input.scroll);
    }
    if (input.resetCamera && !io.WantCaptureKeyboard) {
        camera.reset();
    }
    input.dragX = 0.0f;
    input.dragY = 0.0f;
    input.scroll = 0.0f;
    input.resetCamera = false;
}

void showModelInfo(GLFWwindow* window, const ViewerModel& model)
{
    const std::string title = "Adaptive Cluster Rendering - " + model.name;
    glfwSetWindowTitle(window, title.c_str());
    std::cout << model.name << " | vertices: " << model.scene.mesh.vertices.size()
              << " | triangles: " << model.scene.dataset.indices.size() / 3
              << " | clusters: " << model.scene.dataset.clusters.size() << std::endl;
}

enum class ModelAction { None, Open, Save, Mock };

ModelAction drawPanel(ViewerModel& model, bool& clusterColors, bool& showBounds, int& boundsCluster,
                      const std::string& status)
{
    ModelAction action = ModelAction::None;
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f));
    ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f));
    if (ImGui::Begin("Model", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", model.name.c_str());
        ImGui::Text("Vertices: %zu", model.scene.mesh.vertices.size());
        ImGui::Text("Triangles: %zu", model.scene.dataset.indices.size() / 3);
        ImGui::Text("Clusters: %zu", model.scene.dataset.clusters.size());
        ImGui::Text("Triangles / cluster: <= %u", model.scene.maxTrianglesPerCluster);
        ImGui::Separator();
        ImGui::Checkbox("Cluster colors", &clusterColors);
        ImGui::Checkbox("Cluster bounds", &showBounds);
        if (showBounds) {
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##bounds", &boundsCluster, -1,
                             static_cast<int>(model.scene.dataset.clusters.size() - 1), "Cluster %d");
            ImGui::TextUnformatted("-1: all cluster bounds");
        }
        if (ImGui::Button("Reset camera")) {
            model.camera.reset();
        }
#ifdef _WIN32
        if (ImGui::Button("Open OBJ / ACR...")) action = ModelAction::Open;
        if (ImGui::Button("Save ACR...")) action = ModelAction::Save;
        ImGui::SameLine();
#endif
        if (ImGui::Button("Load mock")) action = ModelAction::Mock;
        if (!status.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", status.c_str());
        }
    }
    ImGui::End();
    return action;
}

#ifdef _WIN32
std::filesystem::path chooseSceneFile(GLFWwindow* window, bool save)
{
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = glfwGetWin32Window(window);
    dialog.lpstrFilter = save ? L"Cluster scene (*.acr)\0*.acr\0" :
                               L"Scene (*.obj;*.acr)\0*.obj;*.acr\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    dialog.lpstrDefExt = save ? L"acr" : L"obj";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) {
        return path;
    }

    const DWORD error = CommDlgExtendedError();
    if (error != 0) {
        throw std::runtime_error("Could not open the file picker (error " + std::to_string(error) + ").");
    }
    return {};
}
#endif

} // namespace

int runViewer(SceneData scene, std::string modelName, ViewerOptions options)
{
    auto model = makeModel(std::move(scene), std::move(modelName));
    const auto smokeOriginal = options.smokeTest ? std::optional<SceneData>(model.scene) : std::nullopt;
    const auto originalName = model.name;
    ViewerInput input;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, options.smokeTest ? GLFW_FALSE : GLFW_TRUE);
    std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(960, 640, "Adaptive Cluster Rendering", nullptr, nullptr), glfwDestroyWindow);
    if (!window) {
        return 1;
    }

    glfwSetWindowUserPointer(window.get(), &input);
    glfwSetMouseButtonCallback(window.get(), handleMouseButton);
    glfwSetCursorPosCallback(window.get(), handleCursor);
    glfwSetScrollCallback(window.get(), handleScroll);
    glfwSetKeyCallback(window.get(), handleKey);

    ViewerUi ui(window.get());
    VulkanRenderer renderer;
    renderer.initialize(window.get());
    renderer.uploadScene(model.scene);
    if (options.verifyGpu || options.smokeTest) renderer.verifyUpload(model.scene);
    showModelInfo(window.get(), model);

    bool clusterColors = options.clusterColors;
    bool showBounds = options.showBounds;
    int boundsCluster = -1;
    std::string status;
    unsigned frame = 0;
    auto replaceModel = [&](SceneData replacement, const std::string& name) {
        auto next = makeModel(std::move(replacement), name);
        renderer.uploadScene(next.scene);
        if (options.verifyGpu || options.smokeTest) renderer.verifyUpload(next.scene);
        model = std::move(next);
        boundsCluster = -1;
        showModelInfo(window.get(), model);
    };
    while (!glfwWindowShouldClose(window.get())) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window.get())) {
            break;
        }
        if (options.smokeTest && frame == 4) {
            replaceModel(makeMockScene(), "Mock (4 clusters)");
            clusterColors = true;
            showBounds = true;
        } else if (options.smokeTest && frame == 8) {
            replaceModel(*smokeOriginal, originalName);
            boundsCluster = 0;
        }
        if (!renderer.prepareFrame(window.get())) {
            input = {};
            glfwWaitEvents();
            continue;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        updateCamera(input, model.camera);
        const auto action = drawPanel(model, clusterColors, showBounds, boundsCluster, status);
        ImGui::Render();
        if (renderer.drawFrame(model.camera, clusterColors, showBounds, boundsCluster, ImGui::GetDrawData())) ++frame;
        if (options.smokeTest && frame >= 12) break;

        if (action != ModelAction::None) {
            input = {};
            try {
                if (action == ModelAction::Mock) {
                    replaceModel(makeMockScene(), "Mock (4 clusters)");
                    status.clear();
                }
#ifdef _WIN32
                else {
                    const auto path = chooseSceneFile(window.get(), action == ModelAction::Save);
                    if (!path.empty()) {
                        if (action == ModelAction::Save) {
                            saveScene(path, model.scene);
                            status = "Saved " + path.filename().u8string();
                            std::cout << "Saved scene: " << path.u8string() << std::endl;
                        } else {
                            replaceModel(loadScene(path), path.filename().u8string());
                            status.clear();
                        }
                    }
                }
#endif
            } catch (const std::exception& failure) {
                status = failure.what();
                std::cerr << status << '\n';
            }
        }
    }
    if (options.smokeTest) {
        if (frame < 12) throw std::runtime_error("GPU smoke test ended before all modes were submitted.");
        std::cout << "GPU smoke passed: Draw All, cluster colors, SSBO bounds, scene replacement." << std::endl;
    }
    return 0;
}
