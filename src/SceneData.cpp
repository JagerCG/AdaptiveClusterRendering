#include "SceneData.h"

#include "ObjLoader.h"
#include <glm/common.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
constexpr std::array<char, 8> kMagic{'A', 'C', 'R', 'M', '1', '\r', '\n', '\0'};
constexpr uint32_t kHeaderBytes = 64;
constexpr uint32_t kVertexBytes = 32;
constexpr uint32_t kClusterBytes = 48;
constexpr uint64_t kMaxFileBytes = 512ull * 1024 * 1024;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool finite(glm::vec3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void writeU32(std::ostream& out, uint32_t value)
{
    const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8),
                          static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    out.write(bytes, 4);
}

uint32_t readU32(std::istream& in)
{
    unsigned char bytes[4]{};
    in.read(reinterpret_cast<char*>(bytes), 4);
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

void writeFloat(std::ostream& out, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    writeU32(out, bits);
}

float readFloat(std::istream& in)
{
    const uint32_t bits = readU32(in);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

void writeVec3(std::ostream& out, glm::vec3 value)
{
    writeFloat(out, value.x); writeFloat(out, value.y); writeFloat(out, value.z);
}

glm::vec3 readVec3(std::istream& in)
{
    glm::vec3 value;
    value.x = readFloat(in); value.y = readFloat(in); value.z = readFloat(in);
    return value;
}

uint64_t fileBytes(uint32_t vertices, uint32_t sourceIndices, uint32_t indices, uint32_t clusters)
{
    return kHeaderBytes + uint64_t(vertices) * (kVertexBytes + 4) + uint64_t(sourceIndices) * 4 +
           uint64_t(indices) * 4 + uint64_t(clusters) * kClusterBytes;
}

using Triangle = std::array<uint32_t, 3>;
std::vector<Triangle> triangles(const std::vector<uint32_t>& indices)
{
    std::vector<Triangle> result;
    result.reserve(indices.size() / 3);
    for (size_t i = 0; i < indices.size(); i += 3) {
        Triangle t{indices[i], indices[i + 1], indices[i + 2]};
        // Canonical cyclic rotation preserves winding and handles duplicate indices.
        const Triangle b{t[1], t[2], t[0]}, c{t[2], t[0], t[1]};
        result.push_back(std::min(t, std::min(b, c)));
    }
    std::sort(result.begin(), result.end());
    return result;
}

SceneData readScene(const std::filesystem::path& path)
{
    const uint64_t size = std::filesystem::file_size(path);
    require(size >= kHeaderBytes && size <= kMaxFileBytes, "Invalid ACR file size (M1 limit: 512 MiB).");
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "Cannot open ACR file.");
    in.exceptions(std::ios::failbit | std::ios::badbit);
    std::array<char, 8> magic{};
    in.read(magic.data(), magic.size());
    require(magic == kMagic, "Invalid ACR file signature.");
    require(readU32(in) == kSceneFileVersion, "Unsupported ACR file version.");
    require(readU32(in) == kHeaderBytes, "Unsupported ACR header size.");
    const uint32_t vertexCount = readU32(in);
    const uint32_t sourceCount = readU32(in);
    const uint32_t indexCount = readU32(in);
    const uint32_t clusterCount = readU32(in);
    const uint32_t capacity = readU32(in);
    require(readU32(in) == kVertexBytes && readU32(in) == kClusterBytes, "Unsupported ACR record layout.");
    require(readU32(in) == 0, "Unsupported ACR flags.");
    require(readU32(in) == vertexCount, "Invalid ACR position ID count.");
    for (int i = 0; i < 3; ++i) require(readU32(in) == 0, "Nonzero ACR reserved header field.");
    require(vertexCount > 0 && sourceCount > 0 && sourceCount % 3 == 0 && indexCount == sourceCount &&
            clusterCount > 0 && clusterCount <= indexCount / 3, "Invalid ACR element counts.");
    require(capacity > 0 && capacity <= std::numeric_limits<uint32_t>::max() / 3, "Invalid cluster capacity.");
    // Check lengths BEFORE allocating anything controlled by the file.
    require(fileBytes(vertexCount, sourceCount, indexCount, clusterCount) == size,
            "ACR payload size does not match its header (truncated or trailing data).");
    SceneData scene;
    scene.maxTrianglesPerCluster = capacity;
    scene.mesh.vertices.resize(vertexCount);
    scene.mesh.indices.resize(sourceCount);
    scene.dataset.indices.resize(indexCount);
    scene.mesh.positionIds.resize(vertexCount);
    scene.dataset.clusters.resize(clusterCount);
    for (auto& v : scene.mesh.vertices) {
        v.position = readVec3(in); v.normal = readVec3(in);
        v.texcoord.x = readFloat(in); v.texcoord.y = readFloat(in);
    }
    for (auto& i : scene.mesh.indices) i = readU32(in);
    for (auto& i : scene.dataset.indices) i = readU32(in);
    for (auto& id : scene.mesh.positionIds) id = readU32(in);
    for (uint32_t id = 0; id < clusterCount; ++id) {
        auto& c = scene.dataset.clusters[id];
        c.boundsMin = readVec3(in);
        require(readU32(in) == 0, "Invalid ACR bounds padding.");
        c.boundsMax = readVec3(in);
        require(readU32(in) == 0, "Invalid ACR bounds padding.");
        c.firstIndex = readU32(in); c.indexCount = readU32(in);
        require(readU32(in) == id && readU32(in) == 0, "Invalid ACR cluster ID or reserved field.");
    }
    validateScene(scene);
    return scene;
}
} // namespace

void validateScene(const SceneData& scene)
{
    const auto& mesh = scene.mesh;
    const auto& data = scene.dataset;
    const auto max32 = std::numeric_limits<uint32_t>::max();
    require(!mesh.vertices.empty() && mesh.vertices.size() <= max32, "Invalid vertex count.");
    require(mesh.positionIds.size() == mesh.vertices.size(), "A position ID is required for each vertex.");
    require(!mesh.indices.empty() && mesh.indices.size() % 3 == 0 && mesh.indices.size() <= max32 &&
            data.indices.size() == mesh.indices.size(), "Invalid scene triangle counts.");
    require(!data.clusters.empty() && data.clusters.size() <= data.indices.size() / 3, "Invalid cluster count.");
    require(scene.maxTrianglesPerCluster > 0 && scene.maxTrianglesPerCluster <= max32 / 3,
            "Invalid cluster triangle capacity.");
    for (const auto& v : mesh.vertices) {
        require(finite(v.position) && finite(v.normal) && std::isfinite(v.texcoord.x) && std::isfinite(v.texcoord.y),
                "Scene contains a non-finite vertex attribute.");
    }
    for (const auto* list : {&mesh.indices, &data.indices}) {
        for (uint32_t index : *list) require(index < mesh.vertices.size(), "Scene index is out of range.");
    }
    uint64_t next = 0;
    for (const auto& c : data.clusters) {
        require(c.firstIndex == next && c.indexCount > 0 && c.indexCount % 3 == 0 &&
                c.indexCount / 3 <= scene.maxTrianglesPerCluster && next + c.indexCount <= data.indices.size(),
                "Cluster ranges must partition the index buffer and respect triangle capacity.");
        require(finite(c.boundsMin) && finite(c.boundsMax), "Non-finite cluster bounds.");
        for (int axis = 0; axis < 3; ++axis) require(c.boundsMin[axis] <= c.boundsMax[axis], "Inverted cluster bounds.");
        for (uint64_t i = next; i < next + c.indexCount; ++i) {
            const auto p = mesh.vertices[data.indices[static_cast<size_t>(i)]].position;
            for (int axis = 0; axis < 3; ++axis) {
                const float tolerance = 1e-5f * std::max(1.0f, std::abs(p[axis]));
                require(p[axis] >= c.boundsMin[axis] - tolerance && p[axis] <= c.boundsMax[axis] + tolerance,
                        "Cluster bounds do not contain their geometry.");
            }
        }
        next += c.indexCount;
    }
    require(next == data.indices.size(), "Cluster ranges leave indices uncovered.");
    require(triangles(mesh.indices) == triangles(data.indices), "Cluster indices changed source triangles or winding.");
}

SceneData buildScene(MeshSource mesh, uint32_t maxTrianglesPerCluster)
{
    auto dataset = buildClusters(mesh, maxTrianglesPerCluster);
    SceneData result{std::move(mesh), std::move(dataset), maxTrianglesPerCluster};
    validateScene(result);
    return result;
}

SceneData makeMockScene()
{
    // Four hand-authored tetrahedra: fixed IDs/ranges/bounds, independent of the
    // clustering algorithm so Track B/C can work before Track A is available.
    SceneData scene;
    scene.maxTrianglesPerCluster = 4;
    constexpr uint32_t faces[] = {0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3};
    const glm::vec3 local[] = {{-.6f, -.5f, -.5f}, {.6f, -.5f, -.5f}, {0, -.5f, .6f}, {0, .7f, 0}};
    for (uint32_t id = 0; id < 4; ++id) {
        const glm::vec3 center{(id % 2 ? 1.f : -1.f), (id / 2 ? .9f : -.9f), 0};
        const uint32_t base = static_cast<uint32_t>(scene.mesh.vertices.size());
        for (uint32_t v = 0; v < 4; ++v) {
            scene.mesh.vertices.push_back({local[v] + center, {0, 1, 0}, {0, 0}});
            scene.mesh.positionIds.push_back(base + v);
        }
        for (uint32_t v : faces) scene.mesh.indices.push_back(base + v);
        scene.dataset.clusters.push_back({id * 12, 12, center + glm::vec3(-.6f, -.5f, -.5f),
                                         center + glm::vec3(.6f, .7f, .6f)});
    }
    scene.dataset.indices = scene.mesh.indices;
    validateScene(scene);
    return scene;
}

SceneData loadScene(const std::filesystem::path& path)
{
    auto extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".obj") return buildScene(loadObj(path));
    if (extension == ".acr") return readScene(path);
    throw std::runtime_error("Unsupported scene file; choose .obj or .acr.");
}

void saveScene(const std::filesystem::path& path, const SceneData& scene)
{
    validateScene(scene);
    const auto vertices = static_cast<uint32_t>(scene.mesh.vertices.size());
    const auto sourceIndices = static_cast<uint32_t>(scene.mesh.indices.size());
    const auto indices = static_cast<uint32_t>(scene.dataset.indices.size());
    const auto clusters = static_cast<uint32_t>(scene.dataset.clusters.size());
    require(fileBytes(vertices, sourceIndices, indices, clusters) <= kMaxFileBytes, "ACR exceeds the M1 512 MiB limit.");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    require(bool(out), "Cannot create ACR output file.");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.write(kMagic.data(), kMagic.size());
    for (uint32_t value : {kSceneFileVersion, kHeaderBytes, vertices, sourceIndices, indices, clusters,
                           scene.maxTrianglesPerCluster, kVertexBytes, kClusterBytes, 0u, vertices, 0u, 0u, 0u}) {
        writeU32(out, value);
    }
    for (const auto& v : scene.mesh.vertices) {
        writeVec3(out, v.position); writeVec3(out, v.normal);
        writeFloat(out, v.texcoord.x); writeFloat(out, v.texcoord.y);
    }
    for (uint32_t i : scene.mesh.indices) writeU32(out, i);
    for (uint32_t i : scene.dataset.indices) writeU32(out, i);
    for (uint32_t id : scene.mesh.positionIds) writeU32(out, id);
    for (uint32_t id = 0; id < clusters; ++id) {
        const auto& c = scene.dataset.clusters[id];
        writeVec3(out, c.boundsMin); writeU32(out, 0);
        writeVec3(out, c.boundsMax); writeU32(out, 0);
        writeU32(out, c.firstIndex); writeU32(out, c.indexCount); writeU32(out, id); writeU32(out, 0);
    }
    out.close();
}
