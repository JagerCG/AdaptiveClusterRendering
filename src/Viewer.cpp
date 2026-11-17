#include "Viewer.h"

#include "ClusterBuilder.h"
#include "MeshSource.h"
#include "SceneData.h"
#include "SceneDiagnostics.h"
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
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
    LoadedScene loaded;
    OrbitCamera camera;
    std::string name;
    std::optional<SceneSaveResult> lastSave;
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

ViewerModel makeModel(LoadedScene loaded)
{
    validateScene(loaded.scene);
    auto camera = frameMesh(loaded.scene.mesh);
    auto name = loaded.source.path.empty() ? std::string(sceneSourceName(loaded.source.kind)) :
                                           loaded.source.path.filename().u8string();
    return {std::move(loaded), camera, std::move(name), std::nullopt};
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

void showModelInfo(GLFWwindow* window, const ViewerModel& model, const SceneUploadStats& upload)
{
    const std::string title = "Adaptive Cluster Rendering - " + model.name;
    glfwSetWindowTitle(window, title.c_str());
    printSceneSummary(std::cout, model.loaded);
    std::cout << "Resident allocation bytes (mesh + clusters): " << upload.residentAllocationBytes
              << "\nCPU upload + wait (ms): " << upload.cpuUploadMs << "\nGPU buffer copies (ms): ";
    if (upload.gpuTransferMs) std::cout << *upload.gpuTransferMs;
    else std::cout << "N/A (timestamps unsupported)";
    std::cout << std::endl;
}

enum class ModelAction { None, Open, Save, ReloadSaved, Mock, Triangle, Plane, ClusterGrid, HeightPreset, OpenHeight };

ClusterSelection previewSelection(const SceneGpuData& source, int preset)
{
    ClusterSelection selection{source.sceneRevision, ClusterSelectionMode::All, {}};
    if (preset != 0) selection.mode = ClusterSelectionMode::Explicit;
    if (preset == 1 || preset == 3) {
        selection.clusterIds.push_back(0);
        if (source.clusterCount > 1) selection.clusterIds.push_back(source.clusterCount - 1);
        if (preset == 3) std::reverse(selection.clusterIds.begin(), selection.clusterIds.end());
    }
    return selection; // preset 2 is explicitly empty, never All
}

ClusterSelection selectionRequest(const ResolvedClusterSelection& resolved)
{
    ClusterSelection result{resolved.sceneRevision,
        resolved.allClusters ? ClusterSelectionMode::All : ClusterSelectionMode::Explicit, {}};
    if (!resolved.allClusters) {
        for (const auto& range : resolved.ranges) result.clusterIds.push_back(range.sourceClusterId);
    }
    return result;
}

void showBytes(const char* label, uint64_t bytes)
{
    ImGui::Text("%s: %.2f KiB", label, double(bytes) / 1024.0);
    ImGui::SetItemTooltip("%llu bytes", static_cast<unsigned long long>(bytes));
}

void drawStorageInfo(const ViewerModel& model, const SceneUploadStats& upload)
{
    const auto& source = model.loaded.source;
    if (ImGui::CollapsingHeader("Storage", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Source: %s", sceneSourceName(source.kind));
        if (!source.path.empty()) ImGui::SetItemTooltip("%s", source.path.u8string().c_str());
        if (source.fileVersion) {
            ImGui::SameLine();
            ImGui::Text("v%u", *source.fileVersion);
        }
        if (source.fileBytes) showBytes("Source file", *source.fileBytes);
        else ImGui::TextUnformatted("Source file: N/A (generated)");
        showBytes("GPU payload", upload.payload.total());
        showBytes("Resident allocation", upload.residentAllocationBytes);
        ImGui::SetItemTooltip("Mesh + cluster allocations, including alignment. Excludes staging, UI and framebuffer memory.");
        if (ImGui::TreeNode("Payload breakdown")) {
            showBytes("Vertices", upload.payload.vertices);
            showBytes("Render indices", upload.payload.indices);
            showBytes("Cluster records", upload.payload.clusters);
            ImGui::TreePop();
        }
    }
    if (ImGui::CollapsingHeader("Timings (last operation)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("CPU prepare: %.3f ms", source.cpuPrepareMs);
        ImGui::SetItemTooltip("%s; excludes camera setup and GPU upload.", scenePrepareLabel(source.kind));
        ImGui::Text("CPU upload + wait: %.3f ms", upload.cpuUploadMs);
        ImGui::SetItemTooltip("Validation, packing, allocation, staging, submission and waiting. Excludes debug readback.");
        if (upload.gpuTransferMs) ImGui::Text("GPU copies: %.6f ms", *upload.gpuTransferMs);
        else ImGui::TextUnformatted("GPU copies: N/A (unsupported)");
        ImGui::SetItemTooltip("GPU timestamps around vertex/index + cluster copies. Single sample, not a benchmark.");
    }
    if (model.lastSave && ImGui::TreeNode("Last verified save")) {
        ImGui::TextWrapped("%s", model.lastSave->path.filename().u8string().c_str());
        showBytes("ACR v1 file", model.lastSave->fileBytes);
        ImGui::Text("CPU save: %.3f ms", model.lastSave->cpuSaveMs);
        ImGui::Text("CPU reload: %.3f ms", model.lastSave->cpuReloadMs);
        ImGui::TextWrapped("Exact geometry / partition match.");
        ImGui::TreePop();
    }
}

void drawBenchmarkPanel(BenchmarkSession& benchmark, BenchmarkOptions& options,
                        const std::filesystem::path& exported, bool& start, std::string& status)
{
    if (!ImGui::CollapsingHeader("Performance measurement")) return;
    ImGui::TextUnformatted("Build: " ACR_BUILD_CONFIG);
    ImGui::TextWrapped("Fix the view and settings, then measure repeated work. Use Release for performance comparisons.");
    if (benchmark.active()) {
        const auto& report = benchmark.report();
        ImGui::Text("Run %u / %u (%u warmup)", report.completedRuns,
                    report.options.warmup + report.options.samples, report.options.warmup);
        ImGui::TextWrapped("Scene, camera and settings are locked.");
        if (ImGui::Button("Cancel measurement")) {
            benchmark.cancel();
            status = "Measurement cancelled; incomplete samples were not exported.";
        }
    } else {
        int warmup = static_cast<int>(options.warmup), samples = static_cast<int>(options.samples);
        ImGui::SetNextItemWidth(150);
        if (ImGui::InputInt("Warmup runs", &warmup)) options.warmup = static_cast<uint32_t>(std::clamp(warmup, 1, 1000));
        ImGui::SetNextItemWidth(150);
        if (ImGui::InputInt("Measured runs", &samples)) options.samples = static_cast<uint32_t>(std::clamp(samples, 2, 10000));
        const auto label = "Measure (" + std::to_string(options.warmup) + " + " + std::to_string(options.samples) + " runs)";
        if (ImGui::Button(label.c_str())) start = true;
    }
    const auto& report = benchmark.report();
    if (report.complete) {
        ImGui::Separator();
        ImGui::TextWrapped("Last completed run (fixed snapshot)");
        ImGui::TextWrapped("%s | %s | n=%llu", report.context.labels.at("scene").c_str(),
                           report.context.flags.at("generated_geometry") ? "generated" : "source",
                           static_cast<unsigned long long>(report.context.counts.at("segments_per_edge")));
        ImGui::TextWrapped("%s | amplitude %.3f | %llu triangles", report.context.labels.at("height_name").c_str(),
                           report.context.values.at("amplitude"),
                           static_cast<unsigned long long>(report.context.counts.at("drawn_triangles")));
        const auto metric = [&](const char* label, std::optional<double> BenchmarkSample::* member) {
            const auto s = summarizeBenchmark(report, member);
            if (!s.count) { ImGui::Text("%s: N/A", label); return; }
            ImGui::TextUnformatted(label);
            ImGui::Text("  median %.6f ms", s.median);
            ImGui::Text("  min %.6f / p95 %.6f", s.minimum, s.p95);
            ImGui::Text("  max %.6f ms (%u samples)", s.maximum, s.count);
        };
        metric("CPU generate + wait", &BenchmarkSample::cpuGenerateMs);
        metric("GPU generate", &BenchmarkSample::gpuGenerateMs);
        metric("GPU render", &BenchmarkSample::gpuRenderMs);
        ImGui::TextWrapped("Render includes clear, mesh, bounds and this UI; excludes acquisition and presentation waits. Not frame time / FPS.");
        if (!exported.empty()) ImGui::TextWrapped("JSON: %s", exported.u8string().c_str());
    }
}

BenchmarkContext captureBenchmark(const ViewerModel& model, const VulkanRenderer& renderer,
                                  bool generated, bool wireframe, bool colors, bool bounds, int boundsCluster,
                                  const DisplacementSettings& displacement, const std::filesystem::path& heightPath, bool uiRendered)
{
    BenchmarkContext context;
    const auto& device = renderer.deviceProperties();
    const auto& source = renderer.sceneGpuData();
    const auto& selection = renderer.clusterSelection();
    const auto& output = renderer.generatedGeometry();
    const auto& upload = renderer.uploadStats();
    const auto extent = renderer.renderExtent();
    context.labels = {{"created_utc", benchmarkUtcStamp()}, {"build_config", ACR_BUILD_CONFIG}, {"gpu", device.deviceName},
        {"scene", model.name}, {"source_kind", sceneSourceName(model.loaded.source.kind)}, {"source_path", model.loaded.source.path.u8string()},
        {"height_name", renderer.heightField().name}, {"height_path", heightPath.u8string()},
        {"generation_scope", "CPU: plan, reallocate, worklist upload, submit and wait; GPU: dispatch interval. No readback or height upload."},
        {"render_scope", uiRendered ? "Render pass: clear + mesh + optional bounds + ImGui. Excludes acquisition/presentation waits." :
                                      "Render pass: clear + mesh + optional bounds; no ImGui. Excludes acquisition/presentation waits."},
        {"sampling", generated ? "Warmup discarded; fixed input; forced regeneration each run; p95 nearest rank; serialized submissions; no GPU clock lock." :
                                 "Warmup discarded; fixed input; source draw each run; p95 nearest rank; serialized submissions; no GPU clock lock."}};
    context.counts = {{"vendor_id", device.vendorID}, {"device_id", device.deviceID}, {"driver_version_raw", device.driverVersion},
        {"vulkan_api_version_raw", device.apiVersion}, {"timestamp_valid_bits", renderer.timestampValidBits()},
        {"width_pixels", extent.width}, {"height_pixels", extent.height}, {"source_revision", source.sceneRevision},
        {"height_revision", renderer.heightRevision()}, {"segments_per_edge", renderer.edgeSegments()},
        {"source_vertices", source.vertexCount}, {"source_triangles", source.indexCount / 3}, {"source_clusters", source.clusterCount},
        {"selected_clusters", selection.ranges.size()}, {"selected_triangles", selection.triangleCount()},
        {"generated_vertices", generated ? output.vertexCount : 0}, {"generated_triangles", generated ? output.indexCount / 3 : 0},
        {"drawn_triangles", renderer.lastRecordedDraw().triangles}, {"mesh_draw_calls", renderer.lastRecordedDraw().meshDrawCalls},
        {"bounds_instances", renderer.lastRecordedDraw().boundsInstances},
        {"source_payload_bytes", upload.payload.total()}, {"source_allocation_bytes", upload.residentAllocationBytes},
        {"output_payload_bytes", generated ? output.vertexBytes + output.indexBytes : 0},
        {"output_allocation_bytes", generated ? output.allocatedOutputBytes : 0},
        {"worklist_payload_bytes", generated ? output.worklistBytes : 0},
        {"worklist_allocation_bytes", generated ? output.allocatedWorklistBytes : 0},
        {"height_width", renderer.heightField().width}, {"height_height", renderer.heightField().height},
        {"height_allocation_bytes", renderer.heightAllocationBytes()}};
    if (model.loaded.source.fileBytes) context.counts["source_file_bytes"] = *model.loaded.source.fileBytes;
    if (model.loaded.source.fileVersion) context.counts["source_file_version"] = *model.loaded.source.fileVersion;
    context.values = {{"amplitude", displacement.amplitude}, {"bounds_padding", generated ? output.maxDisplacement : 0},
        {"bounds_cluster", boundsCluster}, {"timestamp_period_ns", device.limits.timestampPeriod},
        {"startup_cpu_prepare_ms", model.loaded.source.cpuPrepareMs}, {"startup_cpu_upload_ms", upload.cpuUploadMs}};
    if (upload.gpuTransferMs) context.values["startup_gpu_upload_ms"] = *upload.gpuTransferMs;
    context.flags = {{"generated_geometry", generated}, {"displacement_enabled", displacement.enabled}, {"wireframe", wireframe},
        {"cluster_colors", colors}, {"bounds_enabled", bounds}, {"all_clusters", selection.allClusters},
        {"app_enabled_validation", renderer.validationEnabled()}, {"gpu_timestamps_supported", renderer.supportsTimestamps()},
        {"ui_rendered", uiRendered}, {"readback_excluded", true}, {"reallocates_each_generation", generated}};
    for (const auto& range : selection.ranges) context.selectedClusterIds.push_back(range.sourceClusterId);
    const auto view = model.camera.viewProjection(float(extent.width) / float(extent.height));
    for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) context.viewProjection.push_back(view[column][row]);
    return context;
}

ModelAction drawPanel(ViewerModel& model, bool& clusterColors, bool& showBounds, int& boundsCluster,
                      int& selectionPreset, const ResolvedClusterSelection& selection,
                      bool& computeGeometry, bool& regenerate, const GeneratedGeometry& generated,
                      int& segmentPreset, bool& wireframe, bool wireframeSupported,
                      DisplacementSettings& displacement, int& heightPreset, const HeightField& height, uint64_t heightBytes,
                      const SceneUploadStats& upload, std::string& status,
                      BenchmarkSession& benchmark, BenchmarkOptions& benchmarkOptions,
                      const std::filesystem::path& exported, bool& startBenchmark)
{
    const auto& scene = model.loaded.scene;
    ModelAction action = ModelAction::None;
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f));
    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 0.0f),
                                      ImVec2(360.0f, glm::max(100.0f, ImGui::GetIO().DisplaySize.y - 24.0f)));
    if (ImGui::Begin("Model", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", model.name.c_str());
        ImGui::Text("Vertices: %zu", scene.mesh.vertices.size());
        ImGui::Text("Triangles: %zu", scene.dataset.indices.size() / 3);
        ImGui::Text("Clusters: %zu", scene.dataset.clusters.size());
        ImGui::Text("Triangles / cluster: <= %u", scene.maxTrianglesPerCluster);
        drawBenchmarkPanel(benchmark, benchmarkOptions, exported, startBenchmark, status);
        ImGui::Separator();
        ImGui::BeginDisabled(benchmark.active() || startBenchmark);
        ImGui::Checkbox("Cluster colors", &clusterColors);
        ImGui::BeginDisabled(!wireframeSupported);
        ImGui::Checkbox("Wireframe", &wireframe);
        ImGui::EndDisabled();
        if (!wireframeSupported) ImGui::TextWrapped("Wireframe unavailable on this GPU.");
        ImGui::Checkbox("Cluster bounds", &showBounds);
        if (showBounds) {
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##bounds", &boundsCluster, -1,
                             static_cast<int>(scene.dataset.clusters.size() - 1), "Cluster %d");
            ImGui::TextUnformatted("-1: all selected bounds");
        }
        if (ImGui::Button("Reset camera")) {
            model.camera.reset();
        }
#ifdef _WIN32
        if (ImGui::Button("Open OBJ / ACR...")) action = ModelAction::Open;
        if (ImGui::Button("Save + verify ACR...")) action = ModelAction::Save;
#endif
        if (model.lastSave && ImGui::Button("Reload saved ACR")) action = ModelAction::ReloadSaved;
        if (ImGui::Button("Load mock")) action = ModelAction::Mock;
        if (ImGui::CollapsingHeader("Test inputs", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##fixture", "Load test scene...")) {
                if (ImGui::Selectable("Single triangle")) action = ModelAction::Triangle;
                if (ImGui::Selectable("Continuous UV plane")) action = ModelAction::Plane;
                if (ImGui::Selectable("Fixed cluster grid (2 x 2)")) action = ModelAction::ClusterGrid;
                ImGui::EndCombo();
            }
            ImGui::TextUnformatted("Preview selection");
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##selection", &selectionPreset, "All clusters\0First + last cluster\0Empty list\0Last + first cluster\0");
            ImGui::Text("Selected: %zu / %zu clusters", selection.ranges.size(), scene.dataset.clusters.size());
            ImGui::Text("Selected triangles: %llu", static_cast<unsigned long long>(selection.triangleCount()));
            if (!selection.allClusters && !selection.ranges.empty()) {
                ImGui::Text("Source IDs: %u", selection.ranges.front().sourceClusterId);
                if (selection.ranges.size() > 1) {
                    ImGui::SameLine();
                    ImGui::Text(", %u", selection.ranges.back().sourceClusterId);
                }
            }
            if (selection.ranges.empty()) ImGui::TextWrapped("Empty selection: no mesh or bounds.");
            ImGui::TextWrapped("Storage counts cover the full source; the selection controls what is drawn.");
        }
        if (ImGui::CollapsingHeader("Compute geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("GPU subdivision", &computeGeometry);
            ImGui::TextUnformatted("Segments per edge (n)");
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##segments", &segmentPreset, "1 (copy)\0 2\0 4\0 8\0");
            ImGui::TextWrapped("Each input triangle becomes n x n triangles. Enable Wireframe to see the edges.");
            ImGui::Separator();
            ImGui::Checkbox("Height displacement", &displacement.enabled);
            ImGui::TextUnformatted("Amplitude (model units)");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##amplitude", &displacement.amplitude, 0, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Offset = normalized source normal * amplitude * (2 * height - 1). Gray 0.5 is neutral.");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##heightmap", height.name.c_str())) {
                const char* names[] = {"Waves", "Mound", "Ramp U", "Ramp V", "Neutral (0.5)", "Black (0)", "White (1)"};
                for (int i = 0; i < 7; ++i) if (ImGui::Selectable(names[i], heightPreset == i)) {
                    heightPreset = i; action = ModelAction::HeightPreset;
                }
                ImGui::EndCombo();
            }
#ifdef _WIN32
            if (ImGui::Button("Open height map (.pgm)...")) action = ModelAction::OpenHeight;
#endif
            ImGui::Text("Height image: %u x %u", height.width, height.height);
            showBytes("Height allocation", heightBytes);
            if (!computeGeometry && displacement.enabled) ImGui::TextWrapped("Enable GPU subdivision to display displacement.");
            ImGui::TextWrapped("UVs clamp to [0,1]. Displaced faces use geometric normals. Off / zero / neutral preserves source shading.");
            if (computeGeometry && ImGui::Button("Regenerate output")) regenerate = true;
            if (!computeGeometry) ImGui::TextUnformatted("Drawing: source buffers");
            else if (!generated.valid) ImGui::TextUnformatted("Output: pending generation");
            else {
                ImGui::Text("Drawing: GPU output #%llu", static_cast<unsigned long long>(generated.generationRevision));
                ImGui::Text("Generated level: n=%u", generated.edgeSegments);
                ImGui::Text("Output vertices: %u", generated.vertexCount);
                ImGui::Text("Output triangles: %u", generated.indexCount / 3);
                ImGui::Text("Bounds padding: %.4f", generated.maxDisplacement);
                showBytes("Output allocation", generated.allocatedOutputBytes);
                showBytes("Work list allocation", generated.allocatedWorklistBytes);
                ImGui::Text("CPU generate + wait: %.3f ms", generated.cpuGenerateMs);
                ImGui::SetItemTooltip("Last generation: planning, allocation, upload, submission and wait. Excludes debug readback and frame fence wait.");
                if (generated.gpuGenerateMs) ImGui::Text("GPU dispatch: %.6f ms", *generated.gpuGenerateMs);
                else ImGui::TextUnformatted(generated.indexCount ? "GPU dispatch: N/A (unsupported)" : "GPU dispatch: N/A (empty)");
                ImGui::SetItemTooltip("GPU timestamps around the dispatch. Single sample, not a benchmark.");
                ImGui::TextWrapped("Cached until source, selection, n, height map or displacement changes. Camera and wireframe reuse it.");
            }
        }
        ImGui::EndDisabled();
        if (!status.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", status.c_str());
        }
        drawStorageInfo(model, upload);
    }
    ImGui::End();
    return action;
}

#ifdef _WIN32
std::filesystem::path chooseSceneFile(GLFWwindow* window, bool save, bool height = false)
{
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = glfwGetWin32Window(window);
    dialog.lpstrFilter = height ? L"Linear height map (*.pgm)\0*.pgm\0" : save ? L"Cluster scene (*.acr)\0*.acr\0" :
                               L"Scene (*.obj;*.acr)\0*.obj;*.acr\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    dialog.lpstrDefExt = height ? L"pgm" : save ? L"acr" : L"obj";
    // Start height-map browsing beside the bundled example, independent of cwd.
    std::wstring initialDirectory;
    if (height) {
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        if (length > 0 && length < 32768) {
            const auto directory = std::filesystem::path(executable).parent_path() / "assets" / "heightmaps";
            std::error_code error;
            if (std::filesystem::is_directory(directory, error)) {
                initialDirectory = directory.wstring();
                dialog.lpstrInitialDir = initialDirectory.c_str();
            }
        }
    }
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

int runViewer(LoadedScene loaded, ViewerOptions options)
{
    const bool cliBenchmark = !options.benchmarkPath.empty();
    validateBenchmarkOptions(options.benchmarkOptions);
    auto model = makeModel(std::move(loaded));
    const auto smokeOriginal = options.smokeTest ? std::optional<LoadedScene>(model.loaded) : std::nullopt;
    const auto originalCamera = model.camera;
    if (!options.smokeRoundtripPath.empty()) {
        model.lastSave = saveSceneVerified(options.smokeRoundtripPath, model.loaded.scene);
    }
    ViewerInput input;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, (options.smokeTest || cliBenchmark) ? GLFW_FALSE : GLFW_TRUE);
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
    renderer.setEdgeSegments(options.edgeSegments);
    renderer.setDisplacement(options.displacement);
    if (!options.heightMapPath.empty()) renderer.setHeightField(loadHeightField(options.heightMapPath));
    if (options.wireframe && !renderer.supportsWireframe()) throw std::runtime_error("Wireframe is not supported by this GPU.");
    renderer.uploadScene(model.loaded.scene);
    if (options.verifyGpu || options.smokeTest) renderer.verifyUpload(model.loaded.scene);
    showModelInfo(window.get(), model, renderer.uploadStats());

    bool clusterColors = options.clusterColors;
    bool showBounds = options.showBounds;
    bool computeGeometry = options.computeGeometry;
    bool wireframe = options.wireframe;
    auto displacement = options.displacement;
    int heightPreset = options.heightMapPath.empty() ? 0 : -1;
    int segmentPreset = options.edgeSegments == 8 ? 3 : options.edgeSegments == 4 ? 2 : options.edgeSegments == 2 ? 1 : 0;
    const bool displacementSmoke = options.smokeTest && options.displacement.enabled;
    const bool subdivisionSmoke = options.smokeTest && options.edgeSegments > 1 && !displacementSmoke;
    const unsigned smokeFrameCount = displacementSmoke ? 28u : subdivisionSmoke ? 25u : options.computeGeometry ? 16u : 12u;
    int boundsCluster = -1;
    int selectionPreset = options.selectionPreset;
    int appliedPreset = -1;
    std::string status;
    auto heightPath = options.heightMapPath.empty() ? std::filesystem::path{} : std::filesystem::absolute(options.heightMapPath);
    BenchmarkSession benchmark;
    bool benchmarkRequested = cliBenchmark;
    std::filesystem::path exportedBenchmark;
    uint32_t benchmarkSerial = 0;
    auto benchmarkStarted = std::chrono::steady_clock::now();
    unsigned frame = 0;
    auto replaceModel = [&](LoadedScene replacement, bool preserveView = false) {
        auto next = makeModel(std::move(replacement));
        auto previousSelection = selectionRequest(renderer.clusterSelection());
        if (preserveView) {
            if (!sameSceneData(model.loaded.scene, next.loaded.scene)) {
                throw std::runtime_error("Saved file changed; use Open to load it as a new scene.");
            }
            next.camera = model.camera;
            next.lastSave = model.lastSave;
        }
        renderer.uploadScene(next.loaded.scene);
        model = std::move(next);
        if (options.smokeTest) {
            bool rejected = false;
            try { renderer.setClusterSelection(previousSelection); } catch (const std::exception&) { rejected = true; }
            if (!rejected || renderer.generatedGeometry().valid || !renderer.clusterSelection().allClusters ||
                renderer.sceneGpuData().sceneRevision != previousSelection.sceneRevision + 1) {
                throw std::runtime_error("Source replacement did not invalidate the previous selection revision.");
            }
        }
        if (preserveView) {
            previousSelection.sceneRevision = renderer.sceneGpuData().sceneRevision;
            renderer.setClusterSelection(previousSelection);
        } else {
            boundsCluster = -1;
            selectionPreset = appliedPreset = 0;
        }
        if (options.verifyGpu || options.smokeTest) renderer.verifyUpload(model.loaded.scene);
        showModelInfo(window.get(), model, renderer.uploadStats());
    };
    while (!glfwWindowShouldClose(window.get())) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window.get())) {
            break;
        }
        if (options.smokeTest && frame == 4) {
            replaceModel(makeMockSceneWithStats());
            clusterColors = true;
            showBounds = true;
        } else if (options.smokeTest && frame == 8) {
            auto restored = options.smokeRoundtripPath.empty() ? *smokeOriginal :
                            loadSceneWithStats(options.smokeRoundtripPath);
            if (!sameSceneData(smokeOriginal->scene, restored.scene)) {
                throw std::runtime_error("Smoke save/mock/reload changed the source scene.");
            }
            replaceModel(std::move(restored));
            model.camera = originalCamera;
            boundsCluster = 0;
        } else if (options.smokeTest && frame == 10) {
            const auto previous = renderer.uploadStats();
            const auto previousRevision = renderer.sceneGpuData().sceneRevision;
            const auto previousBuffer = renderer.sceneGpuData().vertexBuffer;
            const auto previousOutputValid = renderer.generatedGeometry().valid;
            const auto previousGeneration = renderer.generatedGeometry().generationRevision;
            auto invalid = model.loaded.scene;
            invalid.dataset.indices[0] = UINT32_MAX;
            bool rejected = false;
            try { renderer.uploadScene(invalid); } catch (const std::exception&) { rejected = true; }
            if (!rejected || renderer.sceneGpuData().vertexBuffer != previousBuffer ||
                renderer.sceneGpuData().sceneRevision != previousRevision ||
                renderer.uploadStats().cpuUploadMs != previous.cpuUploadMs ||
                renderer.uploadStats().gpuTransferMs != previous.gpuTransferMs ||
                renderer.uploadStats().payload.total() != previous.payload.total() ||
                renderer.generatedGeometry().valid != previousOutputValid ||
                renderer.generatedGeometry().generationRevision != previousGeneration) {
                throw std::runtime_error("Rejected upload replaced the valid scene / diagnostics.");
            }
            renderer.verifyUpload(model.loaded.scene);
        } else if (options.smokeTest && options.computeGeometry && frame == 12) {
            // Same geometry/counts, new source revision; cache must still be invalidated.
            replaceModel(model.loaded, true);
        }
        if (options.smokeTest) {
            if (frame == 2 || frame == 5 || frame == 9) selectionPreset = 1;
            if (frame == 3 || frame == 6 || frame == 10) { selectionPreset = 2; showBounds = true; }
            if (frame == 7 || frame == 11) selectionPreset = 0;
            if (options.computeGeometry && frame == 12) selectionPreset = 3;
            if (options.computeGeometry && frame == 14) computeGeometry = false;
            if (options.computeGeometry && frame == 15) computeGeometry = true;
            if (subdivisionSmoke) {
                if (frame == 4 || frame == 17 || frame == 20) segmentPreset = 2;
                if (frame == 8 || frame == 18 || frame == 22) segmentPreset = 3;
                if (frame == 12 || frame == 19) segmentPreset = 0;
                if (frame == 16) segmentPreset = 1;
                if (frame == 16 || frame == 23) selectionPreset = 0;
                if (frame == 20) { selectionPreset = 1; clusterColors = false; }
                if (frame == 21) selectionPreset = 2;
                if (frame == 2 || frame == 18) wireframe = renderer.supportsWireframe();
                if (frame == 24) wireframe = false;
            }
            if (displacementSmoke) {
                const auto useHeight = [&](HeightPreset preset) { renderer.setHeightField(makeHeightField(preset)); };
                if (frame == 0 || frame == 12 || frame == 15) useHeight(HeightPreset::Waves);
                if (frame == 4 || frame == 13) useHeight(HeightPreset::Neutral);
                if (frame == 5) useHeight(HeightPreset::Black);
                if (frame == 6) useHeight(HeightPreset::White);
                if (frame == 8) useHeight(HeightPreset::RampU);
                if (frame == 9) useHeight(HeightPreset::RampV);
                if (frame == 0 || frame == 15) segmentPreset = 1;
                if (frame == 8 || frame == 18 || frame == 26) segmentPreset = 2;
                if (frame == 12 || frame == 13) segmentPreset = 0;
                if (frame == 16) { selectionPreset = 0; displacement.enabled = false; }
                if (frame == 17) { displacement.enabled = true; displacement.amplitude = 0; }
                if (frame == 18) displacement.amplitude = .25f;
                if (frame == 19) wireframe = renderer.supportsWireframe();
                if (frame == 20) { displacement.amplitude = .5f; selectionPreset = 1; clusterColors = false; }
                if (frame == 21) selectionPreset = 2;
                if (frame == 22) {
                    const auto old = renderer.heightRevision();
                    renderer.setHeightField(renderer.heightField()); // same image, new revision
                    if (renderer.heightRevision() != old + 1 || renderer.generatedGeometry().valid)
                        throw std::runtime_error("Height reload did not invalidate the cached output.");
                }
                if (frame == 23) renderer.setHeightField(options.heightMapPath.empty() ?
                    makeHeightField(HeightPreset::RampU) : loadHeightField(options.heightMapPath));
                if (frame == 24) { selectionPreset = 0; segmentPreset = 3; wireframe = false; }
                if (frame == 25) {
                    auto zeroNormals = *smokeOriginal;
                    for (auto& vertex : zeroNormals.scene.mesh.vertices) vertex.normal = glm::vec3(0);
                    replaceModel(std::move(zeroNormals));
                }
                if (frame == 26) {
                    replaceModel(*smokeOriginal);
                    useHeight(HeightPreset::Mound);
                    displacement.amplitude = .3f;
                }
            }
        }
        if ((benchmark.active() || benchmarkRequested) &&
            std::chrono::steady_clock::now() - benchmarkStarted > std::chrono::minutes(5)) {
            if (cliBenchmark) throw std::runtime_error("Benchmark timed out before collecting all samples.");
            benchmark.cancel(); benchmarkRequested = false;
            status = "Measurement timed out; incomplete samples were not exported.";
        }
        if (!renderer.prepareFrame(window.get())) {
            input = {};
            glfwWaitEventsTimeout(.05);
            continue;
        }
        if (benchmark.active() &&
            (renderer.renderExtent().width != benchmark.report().context.counts.at("width_pixels") ||
             renderer.renderExtent().height != benchmark.report().context.counts.at("height_pixels"))) {
            if (cliBenchmark) throw std::runtime_error("Benchmark resolution changed.");
            benchmark.cancel();
            status = "Measurement cancelled after resize; run again at the new resolution.";
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (benchmark.active() || benchmarkRequested) input = {};
        else updateCamera(input, model.camera);
        bool regenerate = options.smokeTest && options.computeGeometry && frame == 1;
        const bool alreadyRequested = benchmarkRequested;
        const auto action = cliBenchmark ? ModelAction::None : drawPanel(model, clusterColors, showBounds, boundsCluster, selectionPreset,
                                      renderer.clusterSelection(), computeGeometry, regenerate, renderer.generatedGeometry(),
                                      segmentPreset, wireframe, renderer.supportsWireframe(),
                                      displacement, heightPreset, renderer.heightField(), renderer.heightAllocationBytes(),
                                      renderer.uploadStats(), status, benchmark, options.benchmarkOptions, exportedBenchmark, benchmarkRequested);
        if (benchmarkRequested && !alreadyRequested) benchmarkStarted = std::chrono::steady_clock::now();
        if (selectionPreset != appliedPreset) {
            renderer.setClusterSelection(previewSelection(renderer.sceneGpuData(), selectionPreset));
            appliedPreset = selectionPreset;
        }
        renderer.setEdgeSegments(1u << segmentPreset);
        renderer.setDisplacement(displacement);
        if (computeGeometry) {
            try {
                const auto previousGeneration = renderer.generatedGeometry().generationRevision;
                const bool hadValidOutput = renderer.generatedGeometry().valid;
                const bool generated = renderer.ensureGeneratedGeometry(regenerate || benchmark.active());
                if (options.smokeTest) {
                    const auto revision = renderer.generatedGeometry().generationRevision;
                    if (generated != (!hadValidOutput || regenerate) ||
                        (generated && revision <= previousGeneration) ||
                        (!generated && revision != previousGeneration)) {
                        throw std::runtime_error("Compute cache failed to reuse / regenerate the current selection.");
                    }
                    // Equivalent requests and rejected requests must both preserve this output.
                    renderer.setClusterSelection(selectionRequest(renderer.clusterSelection()));
                    renderer.setEdgeSegments(1u << segmentPreset);
                    renderer.setDisplacement(displacement);
                    if (displacementSmoke) {
                        const auto heightRevision = renderer.heightRevision();
                        for (float invalidAmplitude : {-1.f, 1001.f, std::numeric_limits<float>::quiet_NaN()}) {
                            bool failed = false;
                            try { renderer.setDisplacement({true, invalidAmplitude}); } catch (const std::exception&) { failed = true; }
                            if (!failed) throw std::runtime_error("Invalid displacement settings were accepted.");
                        }
                        auto invalidHeight = renderer.heightField();
                        invalidHeight.samples[0] = std::numeric_limits<float>::infinity();
                        bool failed = false;
                        try { renderer.setHeightField(invalidHeight); } catch (const std::exception&) { failed = true; }
                        if (!failed || renderer.heightRevision() != heightRevision)
                            throw std::runtime_error("Invalid height image changed the active revision.");
                    }
                    bool invalidLevelRejected = false;
                    try { renderer.setEdgeSegments(3); } catch (const std::exception&) { invalidLevelRejected = true; }
                    auto invalid = selectionRequest(renderer.clusterSelection());
                    invalid.mode = ClusterSelectionMode::Explicit;
                    invalid.clusterIds = {renderer.sceneGpuData().clusterCount};
                    bool rejected = false;
                    try { renderer.setClusterSelection(invalid); } catch (const std::exception&) { rejected = true; }
                    if (!rejected || !invalidLevelRejected || renderer.ensureGeneratedGeometry() ||
                        renderer.generatedGeometry().generationRevision != revision) {
                        throw std::runtime_error("Repeated / rejected selection invalidated the compute cache.");
                    }
                }
                if (generated && !benchmark.active() && (options.verifyGpu || options.smokeTest)) {
                    renderer.verifyGeneratedGeometry(model.loaded.scene);
                    renderer.verifyUpload(model.loaded.scene); // compute must not mutate source / serialized data
                }
            } catch (const std::exception& failure) {
                if (options.smokeTest || cliBenchmark) throw;
                benchmark.cancel(); benchmarkRequested = false;
                computeGeometry = false;
                status = std::string("GPU generation failed; drawing source. ") + failure.what();
                std::cerr << status << '\n';
            }
        }
        ImGui::Render();
        if (renderer.drawFrame(model.camera, clusterColors, showBounds, boundsCluster, computeGeometry, wireframe,
                               cliBenchmark ? nullptr : ImGui::GetDrawData(), benchmark.active())) {
            ++frame;
            if (benchmark.active()) {
                BenchmarkSample sample;
                if (computeGeometry) {
                    sample.generationRevision = renderer.generatedGeometry().generationRevision;
                    sample.cpuGenerateMs = renderer.generatedGeometry().cpuGenerateMs;
                    sample.gpuGenerateMs = renderer.generatedGeometry().gpuGenerateMs;
                }
                sample.gpuRenderMs = renderer.finishRenderMeasurement();
                benchmark.add(sample);
                if (benchmark.report().complete) {
                    try {
                        // Correctness readbacks and cache checks are outside all measured intervals.
                        if (computeGeometry && renderer.ensureGeneratedGeometry())
                            throw std::runtime_error("Benchmark did not leave reusable cached geometry.");
                        if (options.verifyGpu) {
                            renderer.verifyUpload(model.loaded.scene);
                            if (computeGeometry) renderer.verifyGeneratedGeometry(model.loaded.scene);
                        }
                        const auto path = cliBenchmark ? options.benchmarkPath : options.reportDirectory /
                            ("benchmark-" + benchmarkUtcStamp() + "-" + std::to_string(++benchmarkSerial) + ".json");
                        writeBenchmarkReport(path, benchmark.report());
                        exportedBenchmark = std::filesystem::absolute(path);
                        status = "Measurement complete; JSON saved to " + exportedBenchmark.u8string();
                        std::cout << status << '\n';
                        for (const auto& metric : {std::make_pair("CPU generate + wait", &BenchmarkSample::cpuGenerateMs),
                                                  std::make_pair("GPU generate", &BenchmarkSample::gpuGenerateMs),
                                                  std::make_pair("GPU render", &BenchmarkSample::gpuRenderMs)}) {
                            const auto s = summarizeBenchmark(benchmark.report(), metric.second);
                            std::cout << metric.first << " median / p95 (ms): ";
                            if (s.count) std::cout << s.median << " / " << s.p95;
                            else std::cout << "N/A";
                            std::cout << '\n';
                        }
                        std::cout << std::flush;
                    } catch (const std::exception& failure) {
                        if (cliBenchmark) throw;
                        status = std::string("Measurement export failed: ") + failure.what();
                        std::cerr << status << '\n';
                    }
                    if (cliBenchmark) break;
                }
            }
            if (benchmarkRequested) {
                exportedBenchmark.clear(); status.clear();
                benchmark.begin(captureBenchmark(model, renderer, computeGeometry, wireframe, clusterColors, showBounds,
                    boundsCluster, displacement, heightPath, !cliBenchmark), options.benchmarkOptions, computeGeometry,
                    renderer.supportsTimestamps(), renderer.clusterSelection().indexCount > 0);
                benchmarkRequested = false;
                benchmarkStarted = std::chrono::steady_clock::now();
            }
            if (options.smokeTest) {
                const auto& selected = renderer.clusterSelection();
                const auto& recorded = renderer.lastRecordedDraw();
                uint32_t expectedBounds = 0;
                if (showBounds) {
                    for (const auto& range : selected.ranges) {
                        if (boundsCluster < 0 || range.sourceClusterId == static_cast<uint32_t>(boundsCluster)) ++expectedBounds;
                    }
                }
                const auto expectedCalls = selected.ranges.empty() ? 0u :
                    (!clusterColors && (computeGeometry || selected.allClusters) ? 1u : static_cast<uint32_t>(selected.ranges.size()));
                const auto expectedTriangles = selected.triangleCount() * (computeGeometry ? uint64_t(renderer.edgeSegments()) * renderer.edgeSegments() : 1);
                if (recorded.triangles != expectedTriangles || recorded.meshDrawCalls != expectedCalls || recorded.wireframe != wireframe ||
                    recorded.boundsInstances != expectedBounds || recorded.usesGeneratedGeometry != computeGeometry ||
                    recorded.boundsPadding != (computeGeometry ? renderer.generatedGeometry().maxDisplacement : 0)) {
                    throw std::runtime_error("Recorded geometry draws do not match the selected clusters / output route.");
                }
            }
        }
        if (options.smokeTest && frame >= smokeFrameCount) break;

        if (action != ModelAction::None) {
            input = {};
            try {
                if (action == ModelAction::HeightPreset) {
                    renderer.setHeightField(makeHeightField(static_cast<HeightPreset>(heightPreset)));
                    heightPath.clear();
                    status.clear();
                }
                else if (action == ModelAction::Mock) {
                    replaceModel(makeMockSceneWithStats());
                    status.clear();
                }
                else if (action == ModelAction::Triangle || action == ModelAction::Plane || action == ModelAction::ClusterGrid) {
                    const auto fixture = action == ModelAction::Triangle ? TessellationFixture::Triangle :
                        action == ModelAction::Plane ? TessellationFixture::Plane : TessellationFixture::ClusterGrid;
                    replaceModel(makeFixtureSceneWithStats(fixture));
                    status.clear();
                }
                else if (action == ModelAction::ReloadSaved) {
                    replaceModel(loadSceneWithStats(model.lastSave->path), true);
                    status = "Reload matched; camera and cluster selection preserved.";
                }
#ifdef _WIN32
                else {
                    const auto path = chooseSceneFile(window.get(), action == ModelAction::Save, action == ModelAction::OpenHeight);
                    if (!path.empty()) {
                        if (action == ModelAction::OpenHeight) {
                            renderer.setHeightField(loadHeightField(path));
                            heightPath = std::filesystem::absolute(path);
                            heightPreset = -1;
                            status = "Height map loaded: " + path.filename().u8string();
                        }
                        else if (action == ModelAction::Save) {
                            model.lastSave = saveSceneVerified(path, model.loaded.scene);
                            status = "Saved + verified: " + path.filename().u8string() +
                                     ". Geometry and cluster partition match exactly.";
                            std::cout << status << " File bytes: " << model.lastSave->fileBytes << std::endl;
                        } else {
                            replaceModel(loadSceneWithStats(path));
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
    if (cliBenchmark && (!benchmark.report().complete || exportedBenchmark.empty()))
        throw std::runtime_error("Benchmark ended without a complete report.");
    if (options.smokeTest) {
        if (frame < smokeFrameCount) throw std::runtime_error("GPU smoke test ended before all modes were submitted.");
        std::cout << "GPU smoke passed: Draw All, cluster colors, SSBO bounds, scene replacement." << std::endl;
        std::cout << "Storage diagnostics passed: payload/allocation sizes, upload timings, rejection preservation"
                  << (options.smokeRoundtripPath.empty() ? "." : ", disk save/mock/reload equality.") << std::endl;
        std::cout << "Selection preview passed: all / first+last / empty, source-ID bounds, revision invalidation." << std::endl;
        if (options.computeGeometry) std::cout << "Compute geometry passed: readback, compact / reversed selection, empty output, "
            "forced regeneration, cache reuse, source replacement, same-scene reload and source/output draw switching." << std::endl;
        if (subdivisionSmoke) std::cout << "Uniform subdivision passed: n=1/2/4/8, winding, area, attributes, shared edges, settings cache invalidation, "
            "empty-level changes and wireframe switching." << std::endl;
        if (displacementSmoke) std::cout << "Height displacement passed: texture/preset reload, zero/neutral/off, geometric normals, "
            "shared edges, conservative bounds, zero source normals, invalid-input preservation and cache transitions." << std::endl;
    }
    return 0;
}
