#include "ClusterBuilder.h"

#include "MeshSource.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace {

struct EdgeTriangles {
    std::vector<uint32_t> triangles;
    size_t nextCandidate = 0;
};

uint64_t edgeKey(uint32_t a, uint32_t b)
{
    return (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
}

} // namespace

ClusterDataset buildClusters(const MeshSource& mesh, uint32_t maxTrianglesPerCluster)
{
    if (maxTrianglesPerCluster == 0) {
        throw std::invalid_argument("A cluster must be allowed at least one triangle.");
    }
    if (mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
        mesh.indices.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("Clustering requires a nonempty 32-bit triangle index list.");
    }
    if (mesh.positionIds.size() != mesh.vertices.size()) {
        throw std::invalid_argument("Clustering requires a position ID for each vertex.");
    }
    for (uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            throw std::invalid_argument("Mesh index is out of range.");
        }
    }

    const auto triangleCount = static_cast<uint32_t>(mesh.indices.size() / 3);
    std::unordered_map<uint64_t, EdgeTriangles> edges;
    edges.reserve(mesh.indices.size());
    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
        const size_t first = static_cast<size_t>(triangle) * 3;
        for (size_t side = 0; side < 3; ++side) {
            const uint32_t a = mesh.positionIds[mesh.indices[first + side]];
            const uint32_t b = mesh.positionIds[mesh.indices[first + (side + 1) % 3]];
            if (a != b) {
                edges[edgeKey(a, b)].triangles.push_back(triangle);
            }
        }
    }

    ClusterDataset result;
    result.indices.reserve(mesh.indices.size());
    std::vector<bool> assigned(triangleCount, false);
    std::vector<uint32_t> group;
    group.reserve(std::min(triangleCount, maxTrianglesPerCluster));

    for (uint32_t seed = 0; seed < triangleCount; ++seed) {
        if (assigned[seed]) {
            continue;
        }

        group.clear();
        group.push_back(seed);
        assigned[seed] = true;
        for (size_t i = 0; i < group.size() && group.size() < maxTrianglesPerCluster; ++i) {
            const size_t first = static_cast<size_t>(group[i]) * 3;
            for (size_t side = 0; side < 3 && group.size() < maxTrianglesPerCluster; ++side) {
                const uint32_t a = mesh.positionIds[mesh.indices[first + side]];
                const uint32_t b = mesh.positionIds[mesh.indices[first + (side + 1) % 3]];
                if (a == b) {
                    continue;
                }

                auto& edge = edges.at(edgeKey(a, b));
                // Each edge keeps its place so a crowded edge is scanned only once.
                while (edge.nextCandidate < edge.triangles.size() && group.size() < maxTrianglesPerCluster) {
                    const uint32_t neighbor = edge.triangles[edge.nextCandidate++];
                    if (!assigned[neighbor]) {
                        assigned[neighbor] = true;
                        group.push_back(neighbor);
                    }
                }
            }
        }

        const uint32_t firstIndex = static_cast<uint32_t>(result.indices.size());
        glm::vec3 minimum = mesh.vertices[mesh.indices[static_cast<size_t>(seed) * 3]].position;
        glm::vec3 maximum = minimum;
        for (uint32_t triangle : group) {
            const size_t first = static_cast<size_t>(triangle) * 3;
            for (size_t corner = 0; corner < 3; ++corner) {
                const uint32_t index = mesh.indices[first + corner];
                result.indices.push_back(index);
                minimum = glm::min(minimum, mesh.vertices[index].position);
                maximum = glm::max(maximum, mesh.vertices[index].position);
            }
        }
        result.clusters.push_back({firstIndex, static_cast<uint32_t>(group.size() * 3), minimum, maximum});
    }
    return result;
}
