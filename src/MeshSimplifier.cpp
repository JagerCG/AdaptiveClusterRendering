#include "MeshSimplifier.h"

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <iterator>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace {

struct SimplifyVertex {
    glm::dmat4 quadric{0.0};
    std::vector<uint32_t> triangles;
    double error = 0.0;
    uint32_t version = 0;
    bool locked = false;
    bool active = true;
    bool splitNormals = false;
};

struct SimplifyTriangle {
    std::array<uint32_t, 3> vertices;
    std::array<uint32_t, 3> corners;
    bool active = true;
};

struct CollapseEdge {
    uint32_t keep;
    uint32_t remove;
    uint32_t keepVersion;
    uint32_t removeVersion;
    double cost;

    bool operator<(const CollapseEdge& other) const
    {
        if (cost != other.cost) return cost > other.cost;
        if (keep != other.keep) return keep > other.keep;
        return remove > other.remove;
    }
};

class Simplifier {
public:
    Simplifier(const MeshSource& mesh, const std::vector<uint32_t>& lockedPositions) : source_(mesh)
    {
        std::unordered_map<uint32_t, uint32_t> positions;
        std::vector<uint32_t> geometryIds(mesh.vertices.size());
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto [entry, inserted] = positions.try_emplace(mesh.positionIds[i], uint32_t(vertices_.size()));
            const uint32_t id = entry->second;
            geometryIds[i] = id;
            if (inserted) {
                vertices_.emplace_back();
                sourceVertices_.push_back(uint32_t(i));
            } else if (mesh.vertices[i].position != source_.vertices[sourceVertices_[id]].position) {
                throw std::invalid_argument("A position ID refers to different coordinates.");
            }
            const glm::dvec3 a(source_.vertices[sourceVertices_[id]].normal);
            const glm::dvec3 b(mesh.vertices[i].normal);
            const double lengths = glm::length(a) * glm::length(b);
            if (lengths > 0.0 && glm::dot(a, b) < 0.9999 * lengths) {
                vertices_[id].splitNormals = true;
            }
        }
        for (uint32_t positionId : lockedPositions) {
            const auto found = positions.find(positionId);
            if (found != positions.end()) vertices_[found->second].locked = true;
        }
        std::unordered_map<uint64_t, uint32_t> edgeCounts;
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            const std::array<uint32_t, 3> sourceCorners{mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
            const std::array<uint32_t, 3> corners{geometryIds[sourceCorners[0]], geometryIds[sourceCorners[1]],
                                                   geometryIds[sourceCorners[2]]};
            const glm::dvec3 a = position(corners[0]);
            glm::dvec3 normal = glm::cross(position(corners[1]) - a, position(corners[2]) - a);
            const double length = glm::length(normal);
            if (length == 0.0) continue;
            normal /= length;
            const glm::dvec4 plane(normal, -glm::dot(normal, a));
            const glm::dmat4 quadric = glm::outerProduct(plane, plane);
            const auto id = static_cast<uint32_t>(triangles_.size());
            triangles_.push_back({corners, sourceCorners});
            for (size_t side = 0; side < 3; ++side) {
                auto& vertex = vertices_[corners[side]];
                vertex.quadric += quadric;
                vertex.triangles.push_back(id);
                const uint32_t x = corners[side];
                const uint32_t y = corners[(side + 1) % 3];
                const uint64_t key = (uint64_t(std::min(x, y)) << 32) | std::max(x, y);
                ++edgeCounts[key];
            }
        }
        triangleCount_ = triangles_.size();
        if (triangleCount_ == 0) throw std::invalid_argument("Mesh has no nondegenerate triangles.");
        for (const auto& entry : edgeCounts) {
            if (entry.second != 2) {
                vertices_[uint32_t(entry.first >> 32)].locked = true;
                vertices_[uint32_t(entry.first)].locked = true;
            }
        }
        for (const auto& entry : edgeCounts) {
            addEdge(uint32_t(entry.first >> 32), uint32_t(entry.first));
        }
    }

    SimplificationResult run(float ratio)
    {
        const size_t target = std::max(size_t(1), size_t(std::ceil(triangleCount_ * double(ratio))));
        while (triangleCount_ > target && !edges_.empty()) {
            const CollapseEdge edge = edges_.top();
            edges_.pop();
            const auto& keep = vertices_[edge.keep];
            const auto& remove = vertices_[edge.remove];
            if (!keep.active || !remove.active || keep.version != edge.keepVersion ||
                remove.version != edge.removeVersion) continue;
            auto faces = keep.triangles;
            faces.insert(faces.end(), remove.triangles.begin(), remove.triangles.end());
            std::sort(faces.begin(), faces.end());
            faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
            if (canCollapse(edge.keep, edge.remove, faces)) collapse(edge.keep, edge.remove, faces);
        }
        return makeResult();
    }

private:
    glm::dvec3 position(uint32_t id) const { return glm::dvec3(source_.vertices[sourceVertices_[id]].position); }

    void addEdge(uint32_t a, uint32_t b)
    {
        if (a == b || !vertices_[a].active || !vertices_[b].active ||
            (vertices_[a].locked && vertices_[b].locked)) return;
        if (a > b) std::swap(a, b);
        const glm::dmat4 quadric = vertices_[a].quadric + vertices_[b].quadric;
        const glm::dvec4 pa(position(a), 1.0);
        const glm::dvec4 pb(position(b), 1.0);
        const double costA = std::max(0.0, glm::dot(pa, quadric * pa));
        const double costB = std::max(0.0, glm::dot(pb, quadric * pb));
        if (vertices_[b].locked || (!vertices_[a].locked && costB < costA)) {
            std::swap(a, b);
        }
        const double cost = a < b ? costA : costB;
        edges_.push({a, b, vertices_[a].version, vertices_[b].version, cost});
    }

    bool canCollapse(uint32_t keep, uint32_t remove, const std::vector<uint32_t>& faces) const
    {
        std::set<uint32_t> keepNeighbors;
        std::set<uint32_t> removeNeighbors;
        std::set<uint32_t> opposite;
        std::set<std::array<uint32_t, 3>> remaining;
        size_t edgeFaces = 0;
        for (uint32_t id : faces) {
            const auto& face = triangles_[id];
            if (!face.active) continue;
            const auto& corners = face.vertices;
            const bool hasKeep = std::find(corners.begin(), corners.end(), keep) != corners.end();
            const bool hasRemove = std::find(corners.begin(), corners.end(), remove) != corners.end();
            for (uint32_t vertex : corners) {
                if (hasKeep && vertex != keep && vertex != remove) keepNeighbors.insert(vertex);
                if (hasRemove && vertex != keep && vertex != remove) removeNeighbors.insert(vertex);
            }
            if (hasKeep && hasRemove) {
                ++edgeFaces;
                for (uint32_t vertex : corners)
                    if (vertex != keep && vertex != remove) opposite.insert(vertex);
                continue;
            }
            auto replacement = corners;
            for (auto& vertex : replacement) if (vertex == remove) vertex = keep;
            const glm::dvec3 oldNormal = glm::cross(position(corners[1]) - position(corners[0]),
                                                   position(corners[2]) - position(corners[0]));
            const glm::dvec3 newNormal = glm::cross(position(replacement[1]) - position(replacement[0]),
                                                   position(replacement[2]) - position(replacement[0]));
            const double oldArea = glm::length(oldNormal);
            const double newArea = glm::length(newNormal);
            if (newArea <= oldArea * 1e-6 || glm::dot(oldNormal, newNormal) <= 0.1 * oldArea * newArea)
                return false;
            std::sort(replacement.begin(), replacement.end());
            if (!remaining.insert(replacement).second) return false;
        }
        if (edgeFaces != 2 || triangleCount_ <= edgeFaces) return false;
        std::vector<uint32_t> common;
        std::set_intersection(keepNeighbors.begin(), keepNeighbors.end(), removeNeighbors.begin(),
                              removeNeighbors.end(), std::back_inserter(common));
        return std::vector<uint32_t>(opposite.begin(), opposite.end()) == common;
    }

    void collapse(uint32_t keep, uint32_t remove, const std::vector<uint32_t>& faces)
    {
        auto& a = vertices_[keep];
        auto& b = vertices_[remove];
        a.quadric += b.quadric;
        a.splitNormals = a.splitNormals || b.splitNormals;
        a.error = std::max(a.error, b.error + glm::length(position(keep) - position(remove)));
        ++a.version;
        ++b.version;
        b.active = false;
        b.triangles.clear();
        a.triangles.clear();
        std::set<uint32_t> neighbors;
        for (uint32_t id : faces) {
            auto& face = triangles_[id];
            if (!face.active) continue;
            for (auto& vertex : face.vertices) if (vertex == remove) vertex = keep;
            const auto& c = face.vertices;
            if (c[0] == c[1] || c[1] == c[2] || c[2] == c[0]) {
                face.active = false;
                --triangleCount_;
                continue;
            }
            a.triangles.push_back(id);
            for (uint32_t vertex : c) if (vertex != keep) neighbors.insert(vertex);
        }
        for (uint32_t neighbor : neighbors) addEdge(keep, neighbor);
    }

    SimplificationResult makeResult() const
    {
        SimplificationResult result;
        const uint32_t unused = std::numeric_limits<uint32_t>::max();
        std::vector<uint32_t> remap(source_.vertices.size(), unused);
        std::vector<uint32_t> outputGeometry;
        for (const auto& face : triangles_) {
            if (!face.active) continue;
            for (size_t side = 0; side < 3; ++side) {
                const uint32_t id = face.vertices[side];
                const uint32_t corner = face.corners[side];
                if (remap[corner] == unused) {
                    remap[corner] = static_cast<uint32_t>(result.mesh.vertices.size());
                    Vertex vertex = source_.vertices[corner];
                    vertex.position = glm::vec3(position(id));
                    result.mesh.vertices.push_back(vertex);
                    result.mesh.positionIds.push_back(source_.positionIds[sourceVertices_[id]]);
                    outputGeometry.push_back(id);
                    result.vertexError = std::max(result.vertexError, float(vertices_[id].error));
                }
                result.mesh.indices.push_back(remap[corner]);
            }
        }
        std::vector<glm::dvec3> normals(result.mesh.vertices.size(), glm::dvec3(0.0));
        std::vector<glm::dvec3> geometryNormals(vertices_.size(), glm::dvec3(0.0));
        for (size_t i = 0; i < result.mesh.indices.size(); i += 3) {
            auto& a = result.mesh.vertices[result.mesh.indices[i]];
            auto& b = result.mesh.vertices[result.mesh.indices[i + 1]];
            auto& c = result.mesh.vertices[result.mesh.indices[i + 2]];
            const glm::dvec3 normal = glm::cross(glm::dvec3(b.position) - glm::dvec3(a.position),
                                                 glm::dvec3(c.position) - glm::dvec3(a.position));
            normals[result.mesh.indices[i]] += normal;
            normals[result.mesh.indices[i + 1]] += normal;
            normals[result.mesh.indices[i + 2]] += normal;
            geometryNormals[outputGeometry[result.mesh.indices[i]]] += normal;
            geometryNormals[outputGeometry[result.mesh.indices[i + 1]]] += normal;
            geometryNormals[outputGeometry[result.mesh.indices[i + 2]]] += normal;
        }
        for (size_t i = 0; i < normals.size(); ++i) {
            const uint32_t id = outputGeometry[i];
            const glm::dvec3 normal = vertices_[id].locked ? glm::dvec3(result.mesh.vertices[i].normal)
                : (vertices_[id].splitNormals ? normals[i] : geometryNormals[id]);
            const double length = glm::length(normal);
            result.mesh.vertices[i].normal = length > 0.0 ? glm::vec3(normal / length) : glm::vec3(0, 1, 0);
        }
        return result;
    }

    const MeshSource& source_;
    std::vector<uint32_t> sourceVertices_;
    std::vector<SimplifyVertex> vertices_;
    std::vector<SimplifyTriangle> triangles_;
    std::priority_queue<CollapseEdge> edges_;
    size_t triangleCount_ = 0;
};

}

SimplificationResult simplifyMesh(const MeshSource& mesh, float triangleRatio,
                                   const std::vector<uint32_t>& lockedPositions)
{
    if (!std::isfinite(triangleRatio) || triangleRatio <= 0.0f || triangleRatio > 1.0f)
        throw std::invalid_argument("Triangle ratio must be greater than zero and at most one.");
    if (mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
        mesh.positionIds.size() != mesh.vertices.size() ||
        mesh.vertices.size() >= std::numeric_limits<uint32_t>::max() ||
        mesh.indices.size() > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("Invalid mesh for simplification.");
    for (const auto& vertex : mesh.vertices)
        if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) || !std::isfinite(vertex.position.z))
            throw std::invalid_argument("Mesh contains a nonfinite position.");
    for (uint32_t index : mesh.indices)
        if (index >= mesh.vertices.size()) throw std::invalid_argument("Mesh index is out of range.");
    if (triangleRatio == 1.0f) return {mesh, 0.0f};
    return Simplifier(mesh, lockedPositions).run(triangleRatio);
}
