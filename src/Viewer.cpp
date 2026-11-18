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

int runRenderIntegrationChecks(VulkanRenderer& renderer, GLFWwindow* window);
int runShadowCasterChecks(VulkanRenderer& renderer, GLFWwindow* window, const SceneData& scene, OrbitCamera camera);
int runHierarchyChecks(VulkanRenderer& renderer, GLFWwindow* window, const SceneData& scene,
                       const OrbitCamera& camera, const std::filesystem::path& output);

namespace {

struct ViewerInput {
    double cursorX = 0.0;
    double cursorY = 0.0;
    float dragX = 0.0f;
    float dragY = 0.0f;
    float scroll = 0.0f;
    bool dragging = false;
    bool resetCamera = false;
    bool togglePanel = false;
};

struct ViewerModel {
    LoadedScene loaded;
    OrbitCamera camera;
    std::string name;
    std::optional<SceneSaveResult> lastSave;
    int geometryLevel = 0;
};

class ViewerUi {
public:
    explicit ViewerUi(GLFWwindow* window)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef _WIN32
        wchar_t windowsDirectory[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            const auto fontPath = std::filesystem::path(windowsDirectory) / "Fonts" / "msyh.ttc";
            std::error_code error;
            if (std::filesystem::is_regular_file(fontPath, error)) {
                // Use the scalable system font for both controls and Unicode model names.
                ImGui::GetIO().Fonts->AddFontFromFileTTF(fontPath.u8string().c_str(), 19.0f);
            }
        }
#endif
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.FontSizeBase = 19.0f;
        style.WindowPadding = {14, 12};
        style.FramePadding = {8, 4};
        style.ItemSpacing = {8, 6};
        style.WindowRounding = 8;
        style.FrameRounding = 4;
        style.PopupRounding = 4;
        style.GrabRounding = 3;
        style.TabRounding = 4;
        // The preferred Vulkan attachment converts linear UI colors to sRGB at presentation.
        style.Colors[ImGuiCol_WindowBg] = ImColor(3, 5, 8, 252);
        style.Colors[ImGuiCol_PopupBg] = ImColor(6, 9, 13);
        style.Colors[ImGuiCol_Text] = ImColor(238, 244, 245);
        style.Colors[ImGuiCol_TextDisabled] = ImColor(112, 137, 151);
        style.Colors[ImGuiCol_Border] = ImColor(18, 27, 35);
        style.Colors[ImGuiCol_Separator] = style.Colors[ImGuiCol_Border];
        style.Colors[ImGuiCol_FrameBg] = ImColor(13, 20, 28);
        style.Colors[ImGuiCol_Button] = style.Colors[ImGuiCol_FrameBg];
        style.Colors[ImGuiCol_Tab] = style.Colors[ImGuiCol_WindowBg];
        style.Colors[ImGuiCol_Header] = ImColor(13, 31, 35);
        style.Colors[ImGuiCol_TabSelected] = style.Colors[ImGuiCol_Header];
        style.Colors[ImGuiCol_ButtonHovered] = ImColor(21, 43, 49);
        style.Colors[ImGuiCol_FrameBgHovered] = style.Colors[ImGuiCol_ButtonHovered];
        style.Colors[ImGuiCol_HeaderHovered] = style.Colors[ImGuiCol_ButtonHovered];
        style.Colors[ImGuiCol_TabHovered] = style.Colors[ImGuiCol_ButtonHovered];
        style.Colors[ImGuiCol_ButtonActive] = ImColor(19, 61, 54);
        style.Colors[ImGuiCol_FrameBgActive] = style.Colors[ImGuiCol_ButtonActive];
        style.Colors[ImGuiCol_HeaderActive] = style.Colors[ImGuiCol_ButtonActive];
        style.Colors[ImGuiCol_CheckMark] = ImColor(64, 193, 155);
        style.Colors[ImGuiCol_TabSelectedOverline] = style.Colors[ImGuiCol_CheckMark];
        style.Colors[ImGuiCol_NavCursor] = style.Colors[ImGuiCol_CheckMark];
        style.Colors[ImGuiCol_SliderGrab] = ImColor(41, 120, 104);
        style.Colors[ImGuiCol_SliderGrabActive] = style.Colors[ImGuiCol_CheckMark];
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
    if (loaded.source.kind == SceneSourceKind::ShadowCasters) {
        camera = OrbitCamera({0, 0, 0}, 2);
        camera.setPose({0, 0, loaded.source.kind == SceneSourceKind::ShadowCasters ? 1.5f : 3.f});
    }
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
    } else if (key == GLFW_KEY_F1) {
        auto& input = *static_cast<ViewerInput*>(glfwGetWindowUserPointer(window));
        input.togglePanel = !input.togglePanel;
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
    std::cout << "Resident allocation bytes (all geometry + hierarchy): " << upload.residentAllocationBytes
              << "\nCPU upload + wait (ms): " << upload.cpuUploadMs << "\nGPU buffer copies (ms): ";
    if (upload.gpuTransferMs) std::cout << *upload.gpuTransferMs;
    else std::cout << "N/A (timestamps unsupported)";
    std::cout << std::endl;
}

enum class ModelAction {
    None,
    Open,
    Save,
    ReloadSaved,
    BlueMound,
    Mock,
    Triangle,
    Plane,
    ClusterGrid,
    ShadowStage,
    ShadowCasters,
    HeightPreset,
    OpenHeight,
    SaveView,
    OpenView,
    HidePanel
};

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
    if (!ImGui::CollapsingHeader("Storage")) return;
    const auto& source = model.loaded.source;
    ImGui::Text("Source: %s", sceneSourceName(source.kind));
    if (!source.path.empty()) ImGui::SetItemTooltip("%s", source.path.u8string().c_str());
    if (source.fileVersion) {
        ImGui::SameLine();
        ImGui::Text("v%u", *source.fileVersion);
    }
    if (source.fileBytes) showBytes("Source file", *source.fileBytes);
    else ImGui::TextDisabled("Generated in memory");
    showBytes("GPU payload", upload.payload.total());
    showBytes("Resident allocation", upload.residentAllocationBytes);
    ImGui::SetItemTooltip("All resident geometry levels and hierarchy records, including alignment.");
    if (ImGui::TreeNode("Payload breakdown")) {
        showBytes("Vertices", upload.payload.vertices);
        showBytes("Render indices", upload.payload.indices);
        showBytes("Cluster records", upload.payload.clusters);
        showBytes("Hierarchy records", upload.payload.hierarchy);
        ImGui::TreePop();
    }
    if (model.lastSave && ImGui::TreeNode("Last verified save")) {
        ImGui::TextWrapped("%s", model.lastSave->path.filename().u8string().c_str());
        showBytes("Saved ACR", model.lastSave->fileBytes);
        ImGui::Text("CPU save: %.3f ms", model.lastSave->cpuSaveMs);
        ImGui::Text("CPU reload: %.3f ms", model.lastSave->cpuReloadMs);
        ImGui::TextWrapped("Verified geometry, cluster IDs and hierarchy.");
        ImGui::TreePop();
    }
}

void drawBenchmarkPanel(const BenchmarkSession& benchmark, BenchmarkOptions& options,
                        const std::filesystem::path& exported, bool& start)
{
    if (!ImGui::CollapsingHeader("Performance measurement", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextDisabled("Build: " ACR_BUILD_CONFIG);
    if (!benchmark.active()) {
        int warmup = static_cast<int>(options.warmup), samples = static_cast<int>(options.samples);
        ImGui::SetNextItemWidth(116);
        if (ImGui::InputInt("Warmup runs", &warmup)) options.warmup = static_cast<uint32_t>(std::clamp(warmup, 1, 1000));
        ImGui::SetNextItemWidth(116);
        if (ImGui::InputInt("Measured runs", &samples)) options.samples = static_cast<uint32_t>(std::clamp(samples, 2, 10000));
        if (ImGui::Button("Start measurement")) start = true;
        ImGui::SetItemTooltip("Lock the current view and settings, run the benchmark and export JSON.");
    }
    const auto& report = benchmark.report();
    if (!report.complete) return;
    ImGui::SeparatorText("Last completed run");
    ImGui::TextWrapped("%s | %s | n=%llu", report.context.labels.at("scene").c_str(),
                       report.context.flags.at("generated_geometry") ? "GPU output" : "source",
                       static_cast<unsigned long long>(report.context.counts.at("segments_per_edge")));
    ImGui::TextWrapped("%s | amplitude %.3f | %llu triangles", report.context.labels.at("height_name").c_str(),
                       report.context.values.at("amplitude"),
                       static_cast<unsigned long long>(report.context.counts.at("drawn_triangles")));
    if (ImGui::BeginTable("##measurements", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("Median ms", ImGuiTableColumnFlags_WidthStretch, 1.05f);
        ImGui::TableSetupColumn("P95 ms", ImGuiTableColumnFlags_WidthStretch, .9f);
        ImGui::TableHeadersRow();
        const auto metric = [&](const char* label, std::optional<double> BenchmarkSample::* member) {
            const auto sample = summarizeBenchmark(report, member);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            if (sample.count) ImGui::SetItemTooltip("Min %.6f ms | max %.6f ms | %u samples",
                                                    sample.minimum, sample.maximum, sample.count);
            ImGui::TableNextColumn();
            if (sample.count) ImGui::Text("%.4f", sample.median); else ImGui::TextDisabled("N/A");
            ImGui::TableNextColumn();
            if (sample.count) ImGui::Text("%.4f", sample.p95); else ImGui::TextDisabled("N/A");
        };
        metric("CPU gen + wait", &BenchmarkSample::cpuGenerateMs);
        metric("GPU generation", &BenchmarkSample::gpuGenerateMs);
        metric("GPU render", &BenchmarkSample::gpuRenderMs);
        metric("GPU main", &BenchmarkSample::gpuMainMs);
        metric("GPU shadows", &BenchmarkSample::gpuShadowMs);
        metric("GPU caster cull", &BenchmarkSample::gpuShadowCullMs);
        metric("CPU cull + wait", &BenchmarkSample::cpuShadowCullMs);
        metric("CPU frame", &BenchmarkSample::cpuFrameMs);
        ImGui::EndTable();
    }
    if (!exported.empty()) {
        ImGui::TextWrapped("Report: %s", exported.filename().u8string().c_str());
        ImGui::SetItemTooltip("%s", exported.u8string().c_str());
    }
}

BenchmarkContext captureBenchmark(const ViewerModel& model, const VulkanRenderer& renderer,
                                  bool generated, bool wireframe, bool colors, bool bounds, int boundsCluster,
                                  const DisplacementSettings& displacement, const std::filesystem::path& heightPath, bool uiRendered)
{
    BenchmarkContext context;
    const auto& device = renderer.deviceProperties();
    const auto& settings = renderer.renderSettings();
    const auto& source = renderer.sceneGpuData();
    const auto& selection = renderer.clusterSelection();
    const auto& output = renderer.generatedGeometry();
    const auto& upload = renderer.uploadStats();
    const auto extent = renderer.renderExtent();
    context.labels = {{"created_utc", benchmarkUtcStamp()}, {"build_config", ACR_BUILD_CONFIG}, {"gpu", device.deviceName},
        {"scene", model.name}, {"source_kind", sceneSourceName(model.loaded.source.kind)}, {"source_path", model.loaded.source.path.u8string()},
        {"height_name", renderer.heightField().name}, {"height_path", heightPath.u8string()},
        {"generation_scope", "CPU: plan, reallocate, worklist upload, submit and wait; GPU: dispatch interval. No readback or height upload."},
        {"render_scope", uiRendered ? "Total render: optional shadow pass + main clear/mesh/bounds/ImGui. Separate main/shadow intervals; excludes acquisition/presentation waits." :
                                      "Total render: optional shadow pass + main clear/mesh/bounds; no ImGui. Separate main/shadow intervals; excludes acquisition/presentation waits."},
        {"sampling", generated ? "Warmup discarded; fixed input; forced regeneration each run; p95 nearest rank; serialized submissions; no GPU clock lock." :
                                 "Warmup discarded; fixed input; source draw each run; p95 nearest rank; serialized submissions; no GPU clock lock."}};
    context.counts = {{"vendor_id", device.vendorID}, {"device_id", device.deviceID}, {"driver_version_raw", device.driverVersion},
        {"vulkan_api_version_raw", device.apiVersion}, {"timestamp_valid_bits", renderer.timestampValidBits()},
        {"width_pixels", extent.width}, {"height_pixels", extent.height}, {"source_revision", source.sceneRevision},
        {"height_revision", renderer.heightRevision()}, {"segments_per_edge", renderer.edgeSegments()},
        {"source_vertices", source.vertexCount}, {"source_triangles", source.indexCount / 3},
        {"source_clusters", source.clusterCount},
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
        {"ui_rendered", uiRendered}, {"readback_excluded", true}, {"verification_readback_excluded", true},
        {"reallocates_each_generation", generated}};
    context.counts["display_mode"] = static_cast<uint32_t>(settings.display);
    context.counts["geometry_level"] = source.activeLevel;
    context.counts["candidate_clusters"] = renderer.clusterSelection().ranges.size();
    context.counts["candidate_triangles"] = renderer.clusterSelection().triangleCount();
    context.counts["resident_cluster_count"] = source.hierarchy.totalClusters;
    context.flags["camera_depth_available"] = renderer.cameraDepth().view != VK_NULL_HANDLE;
    context.flags["camera_depth_solid"] = renderer.cameraDepth().solid;
    context.labels["cpu_frame_scope"] = "Wall time from frame preparation to present, including generation and shadow waits.";
    context.counts["first_cluster_id"] = source.firstCluster;
    context.counts["shadow_triangles"] = renderer.lastRecordedDraw().shadowTriangles;
    context.counts["shadow_draw_calls"] = renderer.lastRecordedDraw().shadowDrawCalls;
    const auto& draw = renderer.lastRecordedDraw();
    context.counts["shadow_candidate_clusters"] = draw.shadowCandidates;
    context.counts["shadow_caster_clusters"] = draw.shadowCasters;
    context.counts["shadow_frustum_rejected"] = draw.shadowCandidates - draw.shadowCasters;
    context.flags["shadow_culling"] = settings.shadowCulling;
    context.values["shadow_coverage"] = settings.shadowCoverage;
    if (draw.shadowPass) {
        for (const auto& range : renderer.shadowSelection().ranges)
            context.shadowClusterIds.push_back(range.sourceClusterId);
        if (settings.shadowCulling) context.flags["readback_excluded"] = false;
    }
    context.counts["visible_clusters"] = renderer.lastRecordedDraw().visibleClusters;
    context.counts["shadow_map_size"] = VulkanRenderer::shadowMapSize;
    context.values["shadow_bias"] = settings.shadowBias;
    context.values["light_x"] = settings.lightDirection.x;
    context.values["light_y"] = settings.lightDirection.y;
    context.values["light_z"] = settings.lightDirection.z;
    context.values["cpu_max_distance"] = settings.maxDistance;
    context.flags["shadows_enabled"] = settings.shadows;
    context.flags["shadow_pass"] = renderer.lastRecordedDraw().shadowPass;
    context.flags["cpu_frustum_preview"] = settings.frustumPreview;
    context.labels["selection_scope"] = "Fixed level and ordered input list. CPU preview filters main draws only.";
    context.labels["shadow_scope"] = "Directional light processes the input list independently of the main camera.";
    for (const auto& range : selection.ranges) context.selectedClusterIds.push_back(range.sourceClusterId);
    const auto view = model.camera.viewProjection(float(extent.width) / float(extent.height));
    for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) {
        context.viewProjection.push_back(view[column][row]);
        context.lightViewProjection.push_back(renderer.lastLightMatrix()[column][row]);
    }
    return context;
}

enum class PanelTab { Scene, Surface, Shadows, Stats };

PanelTab tabForScene(SceneSourceKind kind, bool generated, const RenderSettings& settings)
{
    if (kind == SceneSourceKind::ShadowStage || kind == SceneSourceKind::ShadowCasters ||
        settings.display == RenderDisplay::ShadowMap) return PanelTab::Shadows;
    if (kind == SceneSourceKind::Acr) return PanelTab::Scene;
    if (generated || kind == SceneSourceKind::ClusterGrid || kind == SceneSourceKind::Plane ||
        kind == SceneSourceKind::Triangle) return PanelTab::Surface;
    return settings.shadows || settings.display == RenderDisplay::Depth ? PanelTab::Shadows : PanelTab::Scene;
}

// Keep paired buttons on one row when their labels fit, including in a narrow window.
void sameLineIfFits(const char* nextLabel, float extraWidth = 0)
{
    const auto& style = ImGui::GetStyle();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float needed = style.ItemSpacing.x + ImGui::CalcTextSize(nextLabel).x + 2 * style.FramePadding.x + extraWidth;
    if (ImGui::GetItemRectMax().x + needed <= right) ImGui::SameLine();
}

void fieldLabel(const char* text)
{
    ImGui::TextDisabled("%s", text);
    ImGui::SetNextItemWidth(-1);
}

void drawSceneTab(ViewerModel& model, const VulkanRenderer& renderer, bool& showBounds, int& boundsCluster,
                  int& selectionPreset, RenderSettings& settings, ModelAction& action)
{
    const auto& scene = model.loaded.scene;
    const auto& selection = renderer.clusterSelection();
    const auto active = sceneLevel(scene, model.geometryLevel);
    fieldLabel("Demo scene");
    if (ImGui::BeginCombo("##fixture", "Choose a scene...")) {
        if (ImGui::Selectable("Blue mound")) action = ModelAction::BlueMound;
        ImGui::Separator();
        if (ImGui::Selectable("Fixed cluster grid (2 x 2)")) action = ModelAction::ClusterGrid;
        if (ImGui::Selectable("Continuous UV plane")) action = ModelAction::Plane;
        if (ImGui::Selectable("Single triangle")) action = ModelAction::Triangle;
        if (ImGui::Selectable("Off-camera shadow caster")) action = ModelAction::ShadowCasters;
        if (ImGui::Selectable("Shadow stage (receiver + patch)")) action = ModelAction::ShadowStage;
        if (ImGui::Selectable("Load mock")) action = ModelAction::Mock;
        ImGui::EndCombo();
    }
    if (scene.hierarchy.levels.size() > 1) {
        fieldLabel("Stored ACR geometry levels");
        const auto label = [](int i, const AcrLevelRecord& level) {
            return "L" + std::to_string(i) + (level.maxDiameterPixels ? " (distant)" : "") +
                   "  |  " + std::to_string(level.indexCount / 3) + " triangles";
        };
        if (ImGui::BeginCombo("##geometryLevel", label(model.geometryLevel, active).c_str())) {
            for (int i = 0; i < static_cast<int>(scene.hierarchy.levels.size()); ++i) {
                if (ImGui::Selectable(label(i, scene.hierarchy.levels[i]).c_str(), i == model.geometryLevel))
                    model.geometryLevel = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("L0 is the original mesh. All levels are already resident on the GPU.");
    }
#ifdef _WIN32
    ImGui::SeparatorText("Model files");
    if (ImGui::Button("Open OBJ / ACR...")) action = ModelAction::Open;
    sameLineIfFits("Save + verify ACR...");
    if (ImGui::Button("Save + verify ACR...")) action = ModelAction::Save;
#endif
    if (model.lastSave && ImGui::Button("Reload saved ACR")) action = ModelAction::ReloadSaved;
#ifdef _WIN32
    ImGui::SeparatorText("Saved views");
    if (ImGui::Button("Open view...")) action = ModelAction::OpenView;
    sameLineIfFits("Save view...");
    if (ImGui::Button("Save view...")) action = ModelAction::SaveView;
#endif
    ImGui::Spacing();
    drawStorageInfo(model, renderer.uploadStats());
    if (!ImGui::CollapsingHeader("Cluster inspection")) return;
    fieldLabel("Manual selection");
    ImGui::Combo("##selection", &selectionPreset, "All clusters\0First + last cluster\0Empty list\0Last + first cluster\0");
    ImGui::TextWrapped("Selected: %zu / %u clusters, %llu triangles", selection.ranges.size(), active.clusterCount,
                       static_cast<unsigned long long>(selection.triangleCount()));
    if (!selection.allClusters && !selection.ranges.empty()) {
        ImGui::Text("Source IDs: %u", selection.ranges.front().sourceClusterId);
        if (selection.ranges.size() > 1) {
            ImGui::SameLine();
            ImGui::Text(", %u", selection.ranges.back().sourceClusterId);
        }
    }
    ImGui::Checkbox("Cluster bounds", &showBounds);
    if (showBounds) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##bounds", &boundsCluster, -1, static_cast<int>(active.clusterCount - 1), "Cluster %d");
        ImGui::SetItemTooltip("-1 shows all selected bounds.");
    }
    ImGui::SeparatorText("Camera preview");
    ImGui::Checkbox("CPU frustum preview", &settings.frustumPreview);
    fieldLabel("Max distance (model units)");
    ImGui::SliderFloat("##maxDistance", &settings.maxDistance, 0, 20, "%.2f");
    ImGui::SetItemTooltip("Zero disables the distance filter. Both preview filters affect the camera draw only.");
    ImGui::Text("Main view: %u clusters", renderer.lastRecordedDraw().visibleClusters);
}

void drawSurfaceTab(const VulkanRenderer& renderer, bool& computeGeometry, int& segmentPreset,
                    DisplacementSettings& displacement, int& heightPreset, ModelAction& action)
{
    ImGui::Checkbox("GPU subdivision", &computeGeometry);
    ImGui::SetItemTooltip("Generate a uniformly subdivided mesh on the GPU.");
    ImGui::BeginDisabled(!computeGeometry);
    ImGui::TextDisabled("Segments per edge");
    const char* segments[] = {"1", "2", "4", "8"};
    const float width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4);
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        const bool selected = segmentPreset == i;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        }
        if (ImGui::Button(segments[i], {width, 0})) segmentPreset = i;
        if (selected) ImGui::PopStyleColor(4);
        ImGui::SetItemTooltip("%u output triangle%s per input triangle.", 1u << (2 * i), i ? "s" : "");
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Checkbox("Height displacement", &displacement.enabled);
    if (computeGeometry && displacement.enabled) {
        fieldLabel("Height map");
        if (ImGui::BeginCombo("##heightmap", renderer.heightField().name.c_str())) {
            const char* presets[] = {"Waves", "Mound", "Ramp U", "Ramp V", "Neutral (0.5)", "Black (0)", "White (1)"};
            for (int i = 0; i < IM_ARRAYSIZE(presets); ++i) {
                if (ImGui::Selectable(presets[i], heightPreset == i)) {
                    heightPreset = i;
                    action = ModelAction::HeightPreset;
                }
            }
#ifdef _WIN32
            ImGui::Separator();
            if (ImGui::Selectable("Open height map (.pgm)...")) action = ModelAction::OpenHeight;
#endif
            ImGui::EndCombo();
        }
        fieldLabel("Amplitude (model units)");
        ImGui::SliderFloat("##amplitude", &displacement.amplitude, 0, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Offset along the source normal = amplitude x (2 x height - 1). Mid-gray is neutral.");
    }
    ImGui::EndDisabled();
    if (!computeGeometry) ImGui::TextWrapped("Enable GPU subdivision to apply a height map.");
}

void drawShadowsTab(RenderSettings& settings, const SourceDrawStats& draw)
{
    fieldLabel("Display");
    int display = static_cast<int>(settings.display);
    if (ImGui::Combo("##display", &display, "Shaded\0Camera depth (near is bright)\0Shadow map (light view)\0"))
        settings.display = static_cast<RenderDisplay>(display);
    ImGui::Checkbox("Shadows", &settings.shadows);
    ImGui::Checkbox("Cull casters in light view", &settings.shadowCulling);
    ImGui::Text("Shadow casters: %u / %u", draw.shadowCasters, draw.shadowCandidates);
    ImGui::Text("Shadow triangles: %llu", static_cast<unsigned long long>(draw.shadowTriangles));
    ImGui::SeparatorText("Directional light");
    if (ImGui::Button("Light from left")) settings.lightDirection = {-1, .7f, 1};
    sameLineIfFits("Light from right");
    if (ImGui::Button("Light from right")) settings.lightDirection = {1, .7f, 1};
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Shadow settings")) {
        fieldLabel("Shadow coverage");
        ImGui::SliderFloat("##shadowCoverage", &settings.shadowCoverage, .05f, 1.f, "%.2f");
        ImGui::SetItemTooltip("1 covers the full scene from the light.");
        fieldLabel("Shadow bias");
        ImGui::SliderFloat("##shadowBias", &settings.shadowBias, 0, .02f, "%.4f");
    }
}

void drawDiagnostics(const ViewerModel& model, const VulkanRenderer& renderer, bool computeGeometry, bool& regenerate)
{
    if (ImGui::CollapsingHeader("GPU output")) {
        const auto& generated = renderer.generatedGeometry();
        const auto& height = renderer.heightField();
        ImGui::Text("Shared source vertices: %zu", model.loaded.scene.mesh.vertices.size());
        ImGui::Text("Cluster capacity: %u triangles", model.loaded.scene.maxTrianglesPerCluster);
        ImGui::Text("Height image: %u x %u", height.width, height.height);
        showBytes("Height allocation", renderer.heightAllocationBytes());
        if (!computeGeometry) ImGui::TextUnformatted("Drawing source buffers");
        else if (generated.valid) {
            ImGui::Text("GPU output #%llu | n=%u", static_cast<unsigned long long>(generated.generationRevision), generated.edgeSegments);
            ImGui::Text("Output vertices: %u", generated.vertexCount);
            ImGui::Text("Output triangles: %u", generated.indexCount / 3);
            ImGui::Text("Bounds padding: %.4f", generated.maxDisplacement);
            showBytes("Output allocation", generated.allocatedOutputBytes);
            showBytes("Work list allocation", generated.allocatedWorklistBytes);
            ImGui::Text("CPU generate + wait: %.3f ms", generated.cpuGenerateMs);
            if (generated.gpuGenerateMs) ImGui::Text("GPU dispatch: %.6f ms", *generated.gpuGenerateMs);
            else ImGui::TextDisabled("GPU dispatch: N/A");
        } else ImGui::TextUnformatted("GPU output pending");
        if (computeGeometry && ImGui::Button("Regenerate output")) regenerate = true;
    }
    if (ImGui::CollapsingHeader("Timings (last operation)")) {
        const auto& source = model.loaded.source;
        const auto& upload = renderer.uploadStats();
        ImGui::Text("CPU prepare: %.3f ms", source.cpuPrepareMs);
        ImGui::SetItemTooltip("%s; excludes camera setup and GPU upload.", scenePrepareLabel(source.kind));
        ImGui::Text("CPU upload + wait: %.3f ms", upload.cpuUploadMs);
        if (upload.gpuTransferMs) ImGui::Text("GPU copies: %.6f ms", *upload.gpuTransferMs);
        else ImGui::TextDisabled("GPU copies: N/A");
        const auto& draw = renderer.lastRecordedDraw();
        const auto& light = renderer.shadowVisibilityStats();
        if (draw.shadowPass && renderer.renderSettings().shadowCulling && light.gpuCullMs)
            ImGui::Text("GPU caster test: %.4f ms", *light.gpuCullMs);
    }
}

ModelAction drawPanel(ViewerModel& model, const VulkanRenderer& renderer, std::optional<PanelTab>& requestedTab,
                      bool& clusterColors, bool& showBounds, int& boundsCluster, int& selectionPreset,
                      bool& computeGeometry, bool& regenerate, int& segmentPreset, bool& wireframe,
                      DisplacementSettings& displacement, int& heightPreset, std::string& status,
                      BenchmarkSession& benchmark, BenchmarkOptions& benchmarkOptions,
                      const std::filesystem::path& exported, bool& startBenchmark, RenderSettings& settings)
{
    const auto display = ImGui::GetIO().DisplaySize;
    const float margin = std::clamp(std::min(display.x, display.y) * .018f, 4.0f, 12.0f);
    const float width = std::min(std::clamp(display.x * .36f, 320.0f, 368.0f), std::max(1.0f, display.x - 2 * margin));
    const float height = std::clamp(display.y - 2 * margin, 1.0f, 575.0f);
    const bool compact = height < 420 || width < 310;
    ImGui::SetNextWindowPos({margin, margin});
    ImGui::SetNextWindowSize({width, 0});
    ImGui::SetNextWindowSizeConstraints({width, 0}, {width, height});
    auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!compact) flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ModelAction action = ModelAction::None;
    if (ImGui::Begin("Viewer controls", nullptr, flags)) {
        const bool locked = benchmark.active() || startBenchmark;
        ImGui::PushFont(nullptr, 23);
        ImGui::TextUnformatted(model.name.c_str());
        ImGui::PopFont();
        if (!model.loaded.source.path.empty()) ImGui::SetItemTooltip("%s", model.loaded.source.path.u8string().c_str());
        const auto active = sceneLevel(model.loaded.scene, model.geometryLevel);
        if (ImGui::BeginTable("##triangles", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Source triangles");
            ImGui::SetItemTooltip("All source triangles in the active stored level.");
            ImGui::PushFont(nullptr, 25);
            ImGui::Text("%u", active.indexCount / 3);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Drawn triangles");
            ImGui::SetItemTooltip("Triangles submitted in the last camera pass.");
            ImGui::PushFont(nullptr, 25);
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%llu",
                               static_cast<unsigned long long>(renderer.lastRecordedDraw().triangles));
            ImGui::PopFont();
            ImGui::EndTable();
        }
        ImGui::BeginDisabled(locked);
        ImGui::BeginDisabled(!renderer.supportsWireframe());
        ImGui::Checkbox("Wireframe", &wireframe);
        if (!renderer.supportsWireframe() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Wireframe is unavailable on this GPU.");
        ImGui::EndDisabled();
        sameLineIfFits("Cluster colors", ImGui::GetFrameHeight());
        ImGui::Checkbox("Cluster colors", &clusterColors);
        PanelTab tab = requestedTab.value_or(PanelTab::Scene);
        if (ImGui::BeginTabBar("##viewerTabs", ImGuiTabBarFlags_DrawSelectedOverline | ImGuiTabBarFlags_FittingPolicyShrink)) {
            const auto tabItem = [&](PanelTab value, const char* name) {
                if (ImGui::BeginTabItem(name, nullptr, requestedTab == value ? ImGuiTabItemFlags_SetSelected : 0)) {
                    tab = value;
                    ImGui::EndTabItem();
                }
            };
            tabItem(PanelTab::Scene, "Scene");
            tabItem(PanelTab::Surface, "Surface");
            tabItem(PanelTab::Shadows, "Shadows");
            tabItem(PanelTab::Stats, "Stats");
            ImGui::EndTabBar();
            requestedTab.reset();
        }
        ImGui::PushID(static_cast<int>(tab));
        if (!compact) {
            const auto& style = ImGui::GetStyle();
            const float footer = ImGui::GetFrameHeightWithSpacing() + 2 * style.ItemSpacing.y + style.WindowPadding.y + 2;
            const float maxBody = std::max(ImGui::GetFrameHeight(), height - ImGui::GetCursorPosY() - footer);
            ImGui::SetNextWindowSizeConstraints({0, 0}, {width, maxBody});
            ImGui::BeginChild("##tabBody", {0, 0}, ImGuiChildFlags_AutoResizeY);
        }
        if (!status.empty()) {
            ImGui::TextWrapped("%s", status.c_str());
            if (ImGui::SmallButton("Dismiss")) status.clear();
            ImGui::Separator();
        }
        if (renderer.clusterSelection().ranges.empty())
            ImGui::TextWrapped("Empty selection. Choose All clusters in Scene > Cluster inspection.");
        switch (tab) {
        case PanelTab::Scene:
            drawSceneTab(model, renderer, showBounds, boundsCluster, selectionPreset, settings, action);
            break;
        case PanelTab::Surface:
            drawSurfaceTab(renderer, computeGeometry, segmentPreset, displacement, heightPreset, action);
            break;
        case PanelTab::Shadows:
            drawShadowsTab(settings, renderer.lastRecordedDraw());
            break;
        case PanelTab::Stats:
            drawBenchmarkPanel(benchmark, benchmarkOptions, exported, startBenchmark);
            drawDiagnostics(model, renderer, computeGeometry, regenerate);
            break;
        }
        if (!compact) ImGui::EndChild();
        ImGui::PopID();
        ImGui::EndDisabled();
        ImGui::Separator();
        if (benchmark.active()) {
            const auto& report = benchmark.report();
            ImGui::Text("Run %u / %u", report.completedRuns, report.options.warmup + report.options.samples);
            ImGui::SameLine();
            if (ImGui::Button("Cancel measurement")) {
                benchmark.cancel();
                status = "Measurement cancelled.";
            }
        } else {
            ImGui::BeginDisabled(startBenchmark);
            if (ImGui::Button("Reset camera")) model.camera.reset();
            ImGui::SetItemTooltip("R resets the camera. Drag the model to orbit or scroll to zoom.");
            sameLineIfFits("Hide panel (F1)");
            if (ImGui::Button("Hide panel (F1)")) action = ModelAction::HidePanel;
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
    return action;
}

#ifdef _WIN32
std::filesystem::path chooseSceneFile(GLFWwindow* window, const std::filesystem::path& assets,
                                      bool save, bool height = false, bool config = false)
{
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = glfwGetWin32Window(window);
    dialog.lpstrFilter = config ? L"Viewer configuration (*.acrview)\0*.acrview\0" :
                         height ? L"Linear height map (*.pgm)\0*.pgm\0" : save ? L"Cluster scene (*.acr)\0*.acr\0" :
                               L"Scene (*.obj;*.acr)\0*.obj;*.acr\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    dialog.lpstrDefExt = config ? L"acrview" : height ? L"pgm" : save ? L"acr" : L"obj";
    // Start Open dialogs with the examples that ship beside the executable.
    std::wstring initialDirectory;
    if (!save) {
        const auto directory = height ? assets / "heightmaps" : config ? assets / "views" : assets;
        std::error_code error;
        if (std::filesystem::is_directory(directory, error)) {
            initialDirectory = directory.wstring();
            dialog.lpstrInitialDir = initialDirectory.c_str();
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
    if (options.initialView) model.camera.setPose(options.initialView->pose);
    const auto smokeOriginal = options.smokeTest ? std::optional<LoadedScene>(model.loaded) : std::nullopt;
    const auto originalCamera = model.camera;
    if (!options.smokeRoundtripPath.empty()) {
        model.lastSave = saveSceneVerified(options.smokeRoundtripPath, model.loaded.scene);
    }
    ViewerInput input;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE,
                   (options.smokeTest || options.renderSmoke || options.shadowCullSmoke ||
                    cliBenchmark || !options.saveConfigPath.empty() || !options.hierarchyRoundtripPath.empty())
                       ? GLFW_FALSE
                       : GLFW_TRUE);
    std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(options.initialView ? options.initialView->width : 960, options.initialView ? options.initialView->height : 640,
                         "Adaptive Cluster Rendering", nullptr, nullptr), glfwDestroyWindow);
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
    else if (options.initialView && options.initialView->heightPreset >= 0)
        renderer.setHeightField(makeHeightField(static_cast<HeightPreset>(options.initialView->heightPreset)));
    if (options.wireframe && !renderer.supportsWireframe()) throw std::runtime_error("Wireframe is not supported by this GPU.");
    renderer.uploadScene(model.loaded.scene);
    renderer.setHierarchyLevel(options.geometryLevel);
    model.geometryLevel = static_cast<int>(options.geometryLevel);
    renderer.setRenderSettings(options.renderSettings);
    if (!options.hierarchyRoundtripPath.empty())
        return runHierarchyChecks(renderer,window.get(),model.loaded.scene,model.camera,options.hierarchyRoundtripPath);
    if (options.renderSmoke) return runRenderIntegrationChecks(renderer, window.get());
    if (options.shadowCullSmoke) return runShadowCasterChecks(renderer, window.get(), model.loaded.scene, model.camera);
    if (options.verifyGpu || options.smokeTest) renderer.verifyUpload(model.loaded.scene);
    showModelInfo(window.get(), model, renderer.uploadStats());

    bool clusterColors = options.clusterColors;
    bool showBounds = options.showBounds;
    bool computeGeometry = options.computeGeometry;
    bool wireframe = options.wireframe;
    auto renderSettings = options.renderSettings;
    std::optional<PanelTab> requestedTab = tabForScene(model.loaded.source.kind, computeGeometry, renderSettings);
    auto displacement = options.displacement;
    int heightPreset = options.heightMapPath.empty() ? (options.initialView ? options.initialView->heightPreset : 0) : -1;
    int segmentPreset = options.edgeSegments == 8 ? 3 : options.edgeSegments == 4 ? 2 : options.edgeSegments == 2 ? 1 : 0;
    const bool displacementSmoke = options.smokeTest && options.displacement.enabled;
    const bool subdivisionSmoke = options.smokeTest && options.edgeSegments > 1 && !displacementSmoke;
    const unsigned smokeFrameCount = displacementSmoke ? 28u : subdivisionSmoke ? 25u : options.computeGeometry ? 16u : 12u;
    int boundsCluster = options.initialView ? options.initialView->boundsId : -1;
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
    const auto snapshot = [&] {
        ViewConfig v;
        v.source = model.loaded.source.kind;
        v.sourcePath = model.loaded.source.path;
        v.heightPath = heightPath;
        v.render = renderSettings;
        v.pose = model.camera.pose();
        glfwGetWindowSize(window.get(), &v.width, &v.height);
        v.level = static_cast<uint32_t>(model.geometryLevel);
        v.segments = 1u << segmentPreset;
        v.selection = selectionPreset;
        v.boundsId = boundsCluster;
        v.heightPreset = heightPreset;
        v.clusterColors = clusterColors;
        v.bounds = showBounds;
        v.generated = computeGeometry;
        v.wire = wireframe;
        v.hidePanel = options.hidePanel;
        v.displacement = displacement;
        v.sampling = options.benchmarkOptions;
        return v;
    };
    validateViewScene(snapshot(), model.loaded.scene);
    auto replaceModel = [&](LoadedScene replacement, bool preserveView = false, int level = 0) {
        auto next = makeModel(std::move(replacement));
        next.geometryLevel = level;
        auto previousSelection = selectionRequest(renderer.clusterSelection());
        if (preserveView) {
            if (!sameSceneData(model.loaded.scene, next.loaded.scene)) {
                throw std::runtime_error("Saved file changed; use Open to load it as a new scene.");
            }
            next.camera = model.camera;
            next.lastSave = model.lastSave;
            next.geometryLevel = model.geometryLevel;
        }
        renderer.uploadScene(next.loaded.scene);
        renderer.setHierarchyLevel(static_cast<uint32_t>(next.geometryLevel));
        model = std::move(next);
        if (options.smokeTest) {
            bool rejected = false;
            try { renderer.setClusterSelection(previousSelection); } catch (const std::exception&) { rejected = true; }
            if (!rejected || renderer.generatedGeometry().valid || !renderer.clusterSelection().allClusters ||
                renderer.sceneGpuData().sceneRevision != previousSelection.sceneRevision + 1 + (model.geometryLevel != 0)) {
                throw std::runtime_error("Source replacement did not invalidate the previous selection revision.");
            }
        }
        if (preserveView) {
            previousSelection.sceneRevision = renderer.sceneGpuData().sceneRevision;
            renderer.setClusterSelection(previousSelection);
        } else {
            boundsCluster = -1;
            selectionPreset = appliedPreset = 0;
            requestedTab = tabForScene(model.loaded.source.kind, computeGeometry, renderSettings);
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
            replaceModel(std::move(restored), false, static_cast<int>(options.geometryLevel));
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
        const auto frameStart = std::chrono::steady_clock::now();
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
        if (input.togglePanel && !benchmark.active() && !benchmarkRequested) options.hidePanel = !options.hidePanel;
        input.togglePanel = false;
        if (benchmark.active() || benchmarkRequested) input = {};
        else updateCamera(input, model.camera);
        bool regenerate = options.smokeTest && options.computeGeometry && frame == 1;
        const bool alreadyRequested = benchmarkRequested;
        const auto action =
            cliBenchmark || options.hidePanel
                ? ModelAction::None
                : drawPanel(model, renderer, requestedTab, clusterColors, showBounds, boundsCluster, selectionPreset,
                            computeGeometry, regenerate, segmentPreset, wireframe, displacement, heightPreset, status,
                            benchmark, options.benchmarkOptions, exportedBenchmark, benchmarkRequested, renderSettings);
        if (benchmarkRequested && !alreadyRequested) benchmarkStarted = std::chrono::steady_clock::now();
        renderer.setRenderSettings(renderSettings);
        renderer.setEdgeSegments(1u << segmentPreset);
        renderer.setDisplacement(displacement);
        if (static_cast<uint32_t>(model.geometryLevel) != renderer.sceneGpuData().activeLevel) {
            renderer.setHierarchyLevel(static_cast<uint32_t>(model.geometryLevel));
            appliedPreset = -1;
            boundsCluster = -1;
        }
        if (selectionPreset != appliedPreset) {
            renderer.setClusterSelection(previewSelection(renderer.sceneGpuData(), selectionPreset));
            appliedPreset = selectionPreset;
        }
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
                               cliBenchmark || options.hidePanel ? nullptr : ImGui::GetDrawData(), benchmark.active())) {
            ++frame;
            if (!options.saveConfigPath.empty()) {
                saveViewConfig(options.saveConfigPath,snapshot());
                options.saveConfigPath.clear();
                if (!cliBenchmark) break;
            }
            if (benchmark.active()) {
                BenchmarkSample sample;
                sample.cpuFrameMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frameStart).count();
                if (renderer.lastRecordedDraw().shadowPass && renderSettings.shadowCulling) {
                    const auto& light = renderer.shadowVisibilityStats();
                    sample.cpuShadowCullMs = light.cpuMs;
                    sample.gpuShadowCullMs = light.gpuCullMs;
                }
                if (computeGeometry) {
                    sample.generationRevision = renderer.generatedGeometry().generationRevision;
                    sample.cpuGenerateMs = renderer.generatedGeometry().cpuGenerateMs;
                    sample.gpuGenerateMs = renderer.generatedGeometry().gpuGenerateMs;
                }
                sample.gpuRenderMs = renderer.finishRenderMeasurement();
                sample.gpuMainMs = renderer.lastMainMs();
                sample.gpuShadowMs = renderer.lastShadowMs();
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
                        status = "Measurement complete. Report saved.";
                        std::cout << "Measurement complete; JSON saved to " << exportedBenchmark.u8string() << '\n';
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
                benchmark.begin(captureBenchmark(model, renderer, computeGeometry, wireframe, clusterColors,
                                                 showBounds, boundsCluster, displacement, heightPath,
                                                 !cliBenchmark && !options.hidePanel),
                                options.benchmarkOptions, computeGeometry, renderer.supportsTimestamps(),
                                renderer.clusterSelection().indexCount > 0);
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
                if (action == ModelAction::HidePanel) options.hidePanel = true;
                else if (action == ModelAction::BlueMound || action == ModelAction::SaveView || action == ModelAction::OpenView) {
                    auto path = options.assetDirectory / "views" / "m2-grid-mound.acrview";
#ifdef _WIN32
                    if (action != ModelAction::BlueMound)
                        path = chooseSceneFile(window.get(), options.assetDirectory, action == ModelAction::SaveView, false, true);
#endif
                    if (path.empty()) continue;
                    if (action == ModelAction::SaveView)
                        saveViewConfig(path, snapshot());
                    else {
                        const auto v = loadViewConfig(path);
                        auto source = loadViewScene(v);
                        validateViewScene(v, source.scene);
                        if (v.wire && !renderer.supportsWireframe())
                            throw std::runtime_error("Wireframe is unavailable on this GPU.");
                        const auto field = v.heightPath.empty()
                                               ? makeHeightField(static_cast<HeightPreset>(v.heightPreset))
                                               : loadHeightField(v.heightPath);
                        const auto previousField = renderer.heightField();
                        renderer.setHeightField(field);
                        try {
                            replaceModel(std::move(source), false, static_cast<int>(v.level));
                        }
                        catch (...) {
                            renderer.setHeightField(previousField);
                            throw;
                        }
                        model.camera.setPose(v.pose);
                        heightPath = v.heightPath;
                        heightPreset = v.heightPreset;
                        displacement = v.displacement;
                        renderSettings = v.render;
                        clusterColors = v.clusterColors;
                        showBounds = v.bounds;
                        boundsCluster = v.boundsId;
                        selectionPreset = v.selection;
                        appliedPreset = -1;
                        computeGeometry = v.generated;
                        requestedTab = tabForScene(model.loaded.source.kind, computeGeometry, renderSettings);
                        wireframe = v.wire;
                        options.hidePanel = v.hidePanel;
                        segmentPreset = v.segments == 8 ? 3 : v.segments == 4 ? 2 : v.segments == 2 ? 1 : 0;
                        options.benchmarkOptions = v.sampling;
                        int w, h;
                        glfwGetWindowSize(window.get(), &w, &h);
                        if (action != ModelAction::BlueMound && (w != v.width || h != v.height)) {
                            if (glfwGetWindowAttrib(window.get(), GLFW_MAXIMIZED)) glfwRestoreWindow(window.get());
                            glfwSetWindowSize(window.get(), v.width, v.height);
                        }
                    }
                    if (action == ModelAction::BlueMound) status.clear();
                    else status = (action == ModelAction::SaveView ? "Saved view: " : "Restored view: ") +
                                  path.filename().u8string();
                }
                else if (action == ModelAction::HeightPreset) {
                    renderer.setHeightField(makeHeightField(static_cast<HeightPreset>(heightPreset)));
                    heightPath.clear();
                    status.clear();
                }
                else if (action == ModelAction::Mock) {
                    replaceModel(makeMockSceneWithStats());
                    status.clear();
                }
                else if (action == ModelAction::Triangle || action == ModelAction::Plane ||
                         action == ModelAction::ClusterGrid || action == ModelAction::ShadowStage ||
                         action == ModelAction::ShadowCasters) {
                    const auto fixture = action == ModelAction::Triangle      ? TessellationFixture::Triangle
                                         : action == ModelAction::Plane       ? TessellationFixture::Plane
                                         : action == ModelAction::ShadowStage ? TessellationFixture::ShadowStage
                                         : action == ModelAction::ShadowCasters ? TessellationFixture::ShadowCasters
                                                                              : TessellationFixture::ClusterGrid;
                    replaceModel(makeFixtureSceneWithStats(fixture));
                    status.clear();
                }
                else if (action == ModelAction::ReloadSaved) {
                    replaceModel(loadSceneWithStats(model.lastSave->path), true);
                    status = "Reload matched; camera, geometry level and selection preserved.";
                }
#ifdef _WIN32
                else {
                    const auto path = chooseSceneFile(window.get(), options.assetDirectory,
                                                      action == ModelAction::Save, action == ModelAction::OpenHeight);
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
                                     ". Geometry, partition and hierarchy match exactly.";
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
