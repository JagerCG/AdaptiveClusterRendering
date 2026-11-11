#include "Viewer.h"

#include "ClusterBuilder.h"
#include "MeshSource.h"
#include "ObjLoader.h"
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
    MeshSource mesh;
    ClusterDataset dataset;
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

ViewerModel makeModel(MeshSource mesh, std::string name)
{
    auto dataset = buildClusters(mesh);
    auto camera = frameMesh(mesh);
    return {std::move(mesh), std::move(dataset), camera, std::move(name)};
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
    std::cout << model.name << " | vertices: " << model.mesh.vertices.size()
              << " | triangles: " << model.dataset.indices.size() / 3
              << " | clusters: " << model.dataset.clusters.size() << std::endl;
}

bool drawPanel(ViewerModel& model, bool& clusterColors, const std::string& error)
{
    bool openModel = false;
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f));
    ImGui::SetNextWindowSize(ImVec2(248.0f, 0.0f));
    if (ImGui::Begin("Model", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", model.name.c_str());
        ImGui::Text("Vertices: %zu", model.mesh.vertices.size());
        ImGui::Text("Triangles: %zu", model.dataset.indices.size() / 3);
        ImGui::Text("Clusters: %zu", model.dataset.clusters.size());
        ImGui::Separator();
        ImGui::Checkbox("Cluster colors", &clusterColors);
        if (ImGui::Button("Reset camera")) {
            model.camera.reset();
        }
#ifdef _WIN32
        ImGui::SameLine();
        openModel = ImGui::Button("Open OBJ...");
#endif
        if (!error.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", error.c_str());
        }
    }
    ImGui::End();
    return openModel;
}

#ifdef _WIN32
std::filesystem::path chooseObjFile(GLFWwindow* window)
{
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = glfwGetWin32Window(window);
    dialog.lpstrFilter = L"Wavefront OBJ (*.obj)\0*.obj\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    dialog.lpstrDefExt = L"obj";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
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

int runViewer(MeshSource mesh, std::string modelName)
{
    auto model = makeModel(std::move(mesh), std::move(modelName));
    ViewerInput input;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
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
    renderer.uploadMesh(model.mesh, model.dataset.indices);
    showModelInfo(window.get(), model);

    bool clusterColors = false;
    std::string error;
    while (!glfwWindowShouldClose(window.get())) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window.get())) {
            break;
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
        const bool openModel = drawPanel(model, clusterColors, error);
        ImGui::Render();
        renderer.drawFrame(model.camera, model.dataset, clusterColors, ImGui::GetDrawData());

#ifdef _WIN32
        if (openModel) {
            input = {};
            try {
                const auto path = chooseObjFile(window.get());
                if (!path.empty()) {
                    auto replacement = makeModel(loadObj(path), path.filename().u8string());
                    renderer.uploadMesh(replacement.mesh, replacement.dataset.indices);
                    model = std::move(replacement);
                    showModelInfo(window.get(), model);
                    error.clear();
                }
            } catch (const std::exception& failure) {
                error = failure.what();
                std::cerr << error << '\n';
            }
        }
#else
        (void)openModel;
#endif
    }
    return 0;
}
