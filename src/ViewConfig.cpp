#include "ViewConfig.h"
#include "GeometryGenerationPlan.h"
#include "OrbitCamera.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
void validate(const ViewConfig& v)
{
    validateRenderSettings(v.render);
    validateDisplacement(v.displacement);
    validateBenchmarkOptions(v.sampling);
    OrbitCamera({0, 0, 0}, 1).setPose(v.pose);
    const bool disk = v.source == SceneSourceKind::Acr || v.source == SceneSourceKind::Obj;
    if (v.source < SceneSourceKind::Obj || v.source > SceneSourceKind::Occlusion || disk == v.sourcePath.empty() ||
        v.width < 1 || v.height < 1 || v.width > 32768 || v.height > 32768 || !validEdgeSegments(v.segments) ||
        v.selection < 0 || v.selection > 3 || v.boundsId < -1 || v.heightPreset < -1 || v.heightPreset > 6 ||
        ((v.heightPreset == -1) != !v.heightPath.empty()))
        throw std::runtime_error("Invalid saved view settings or asset path.");
}
void key(std::istream& in, const char* expected)
{
    std::string token;
    if (!(in >> token) || token != expected) throw std::runtime_error(std::string("Invalid view field: ") + expected);
}
void checkPath(const std::filesystem::path& path)
{
    if (path.extension() != ".acrview") throw std::runtime_error("Choose a file with the .acrview extension.");
}
} // namespace

void validateViewScene(const ViewConfig& view, const SceneData& scene)
{
    const auto level = sceneLevel(scene, view.level);
    const auto clusters = view.render.adaptiveLod && !scene.hierarchy.levels.empty()
                              ? scene.dataset.clusters.size() + scene.hierarchy.coarse.clusters.size()
                              : level.clusterCount;
    if (view.boundsId >= 0 && size_t(view.boundsId) >= clusters)
        throw std::runtime_error("Saved bounds ID exceeds the selected scene.");
}

LoadedScene loadViewScene(const ViewConfig& v)
{
    switch (v.source) {
    case SceneSourceKind::Obj:
    case SceneSourceKind::Acr: {
        auto loaded = loadSceneWithStats(v.sourcePath);
        if (loaded.source.kind != v.source) throw std::runtime_error("Saved asset type does not match its file.");
        return loaded;
    }
    case SceneSourceKind::Mock:
        return makeMockSceneWithStats();
    case SceneSourceKind::Cube:
        return makeCubeSceneWithStats();
    case SceneSourceKind::Triangle:
        return makeFixtureSceneWithStats(TessellationFixture::Triangle);
    case SceneSourceKind::Plane:
        return makeFixtureSceneWithStats(TessellationFixture::Plane);
    case SceneSourceKind::ClusterGrid:
        return makeFixtureSceneWithStats(TessellationFixture::ClusterGrid);
    case SceneSourceKind::ShadowStage:
        return makeFixtureSceneWithStats(TessellationFixture::ShadowStage);
    case SceneSourceKind::Occlusion:
        return makeFixtureSceneWithStats(TessellationFixture::Occlusion);
    }
    throw std::runtime_error("Unknown saved scene.");
}

ViewConfig loadViewConfig(const std::filesystem::path& path)
{
    checkPath(path);
    std::ifstream in(path);
    in.imbue(std::locale::classic());
    if (!in || std::filesystem::file_size(path) > 1024 * 1024)
        throw std::runtime_error("Cannot read the view configuration.");
    ViewConfig v;
    int version, kind, display;
    std::string source, height;
    key(in, "ACR_VIEW");
    if (!(in >> version) || (version != 1 && version != 2))
        throw std::runtime_error("Unsupported view configuration version.");
    key(in, "source");
    in >> kind >> std::quoted(source);
    key(in, "window");
    in >> v.width >> v.height >> v.hidePanel;
    key(in, "camera");
    in >> v.pose.x >> v.pose.y >> v.pose.z;
    key(in, "lod");
    in >> v.level >> v.render.adaptiveLod >> v.render.lodPixels >> v.render.levelColors;
    key(in, "draw");
    in >> v.clusterColors >> v.render.groupColors >> v.bounds >> v.boundsId >> v.wire >> v.selection >> display;
    key(in, "light");
    in >> v.render.shadows >> v.render.shadowBias >> v.render.lightDirection.x >> v.render.lightDirection.y >>
        v.render.lightDirection.z;
    key(in, "preview");
    in >> v.render.frustumPreview >> v.render.maxDistance;
    key(in, "geometry");
    in >> v.generated >> v.segments >> v.displacement.enabled >> v.displacement.amplitude;
    key(in, "height");
    in >> v.heightPreset >> std::quoted(height);
    key(in, "sample");
    in >> v.sampling.warmup >> v.sampling.samples;
    if (version >= 2) {
        uint32_t mode = 0;
        key(in, "culling");
        in >> mode >> v.render.showHiZ >> v.render.hiZMip >> v.render.showRejectedBounds;
        v.render.culling = static_cast<CullMode>(mode);
    }
    if (!in || (in >> std::ws).peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Malformed view configuration.");
    v.source = static_cast<SceneSourceKind>(kind);
    v.render.display = static_cast<RenderDisplay>(display);
    const auto resolve = [&](const std::string& name) {
        return name.empty()
                   ? std::filesystem::path{}
                   : std::filesystem::absolute(path.parent_path() / std::filesystem::u8path(name)).lexically_normal();
    };
    v.sourcePath = resolve(source);
    v.heightPath = resolve(height);
    validate(v);
    return v;
}

void saveViewConfig(const std::filesystem::path& path, const ViewConfig& v)
{
    checkPath(path);
    validate(v);
    // Resolve relative to the configuration so a project and its assets can be moved together.
    const auto relative = [&](const std::filesystem::path& asset) {
        if (asset.empty()) return std::string{};
        const auto full = std::filesystem::absolute(asset).lexically_normal();
        const auto local = full.lexically_relative(std::filesystem::absolute(path).parent_path());
        return (local.empty() ? full : local).generic_u8string();
    };
    static std::atomic<uint64_t> sequence{0};
    std::filesystem::path directory;
    do {
        directory = path.parent_path() /
                    (".view-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(sequence.fetch_add(1)));
    } while (!std::filesystem::create_directory(directory));
    const auto temp = directory / "view.acrview";
    try {
        std::ofstream out(temp);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<float>::max_digits10);
        const auto& s = v.render;
        out << "ACR_VIEW 2\nsource " << int(v.source) << ' ' << std::quoted(relative(v.sourcePath)) << "\nwindow "
            << v.width << ' ' << v.height << ' ' << v.hidePanel << "\ncamera " << v.pose.x << ' ' << v.pose.y << ' '
            << v.pose.z << "\nlod " << v.level << ' ' << s.adaptiveLod << ' ' << s.lodPixels << ' ' << s.levelColors
            << "\ndraw " << v.clusterColors << ' ' << s.groupColors << ' ' << v.bounds << ' ' << v.boundsId << ' '
            << v.wire << ' ' << v.selection << ' ' << int(s.display) << "\nlight " << s.shadows << ' ' << s.shadowBias
            << ' ' << s.lightDirection.x << ' ' << s.lightDirection.y << ' ' << s.lightDirection.z << "\npreview "
            << s.frustumPreview << ' ' << s.maxDistance << "\ngeometry " << v.generated << ' ' << v.segments << ' '
            << v.displacement.enabled << ' ' << v.displacement.amplitude << "\nheight " << v.heightPreset << ' '
            << std::quoted(relative(v.heightPath)) << "\nsample " << v.sampling.warmup << ' ' << v.sampling.samples
            << "\nculling " << static_cast<uint32_t>(s.culling) << ' ' << s.showHiZ << ' ' << s.hiZMip << ' '
            << s.showRejectedBounds << '\n';
        out.close();
        std::filesystem::rename(temp, path);
    }
    catch (...) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        std::filesystem::remove(directory, ec);
        throw;
    }
    std::error_code ec;
    std::filesystem::remove(directory, ec);
}
