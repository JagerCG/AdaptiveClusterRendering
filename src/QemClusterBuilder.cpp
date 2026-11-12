#include "QemClusterBuilder.h"

#include "MeshSource.h"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Triangle = std::array<uint32_t, 3>;

uint64_t edgeKey(uint32_t a, uint32_t b)
{
    return (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
}

Triangle canonicalTriangle(Triangle triangle)
{
    const Triangle b{triangle[1], triangle[2], triangle[0]};
    const Triangle c{triangle[2], triangle[0], triangle[1]};
    return std::min(triangle, std::min(b, c));
}

struct TriangleKeyHash {
    size_t operator()(const Triangle& triangle) const
    {
        size_t hash = triangle[0];
        hash = hash * 16777619u ^ triangle[1];
        hash = hash * 16777619u ^ triangle[2];
        return hash;
    }
};

struct Quadric {
    std::array<double, 10> values{};

    void addPlane(double a, double b, double c, double d)
    {
        const double plane[] = {a, b, c, d};
        size_t index = 0;
        for (size_t row = 0; row < 4; ++row) {
            for (size_t column = row; column < 4; ++column) {
                values[index++] += plane[row] * plane[column];
            }
        }
    }

    Quadric& operator+=(const Quadric& other)
    {
        for (size_t i = 0; i < values.size(); ++i) values[i] += other.values[i];
        return *this;
    }

    double evaluate(const glm::dvec3& point) const
    {
        const double x = point.x, y = point.y, z = point.z;
        return values[0] * x * x + 2.0 * values[1] * x * y + 2.0 * values[2] * x * z +
               2.0 * values[3] * x + values[4] * y * y + 2.0 * values[5] * y * z +
               2.0 * values[6] * y + values[7] * z * z + 2.0 * values[8] * z + values[9];
    }
};

struct EdgeInfo {
    std::vector<uint32_t> triangles;
    double error = 0.0;
};

struct QemModel {
    std::vector<Triangle> triangles;
    std::vector<uint32_t> vertexPositionIds;
    std::unordered_map<uint64_t, EdgeInfo> edges;
};

bool finite(const glm::dvec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool samePosition(const glm::dvec3& a, const glm::dvec3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool solveOptimalPosition(const Quadric& q, glm::dvec3& result)
{
    double matrix[3][4] = {
        {q.values[0], q.values[1], q.values[2], -q.values[3]},
        {q.values[1], q.values[4], q.values[5], -q.values[6]},
        {q.values[2], q.values[5], q.values[7], -q.values[8]}
    };
    double scale = 0.0;
    for (const auto& row : matrix) {
        for (size_t column = 0; column < 3; ++column) scale = std::max(scale, std::abs(row[column]));
    }
    if (scale == 0.0) return false;

    for (size_t pivot = 0; pivot < 3; ++pivot) {
        size_t best = pivot;
        for (size_t row = pivot + 1; row < 3; ++row) {
            if (std::abs(matrix[row][pivot]) > std::abs(matrix[best][pivot])) best = row;
        }
        if (std::abs(matrix[best][pivot]) <= scale * 1e-12) return false;
        if (best != pivot) {
            for (size_t column = pivot; column < 4; ++column) {
                std::swap(matrix[pivot][column], matrix[best][column]);
            }
        }
        const double divisor = matrix[pivot][pivot];
        for (size_t column = pivot; column < 4; ++column) matrix[pivot][column] /= divisor;
        for (size_t row = 0; row < 3; ++row) {
            if (row == pivot) continue;
            const double factor = matrix[row][pivot];
            for (size_t column = pivot; column < 4; ++column) {
                matrix[row][column] -= factor * matrix[pivot][column];
            }
        }
    }
    result = {matrix[0][3], matrix[1][3], matrix[2][3]};
    return finite(result);
}

double edgeError(const Quadric& a, const Quadric& b, const glm::dvec3& positionA,
                 const glm::dvec3& positionB)
{
    Quadric combined = a;
    combined += b;
    const glm::dvec3 midpoint = (positionA + positionB) * 0.5;
    double best = std::min({combined.evaluate(positionA), combined.evaluate(positionB),
                            combined.evaluate(midpoint)});
    glm::dvec3 optimal;
    if (solveOptimalPosition(combined, optimal)) best = std::min(best, combined.evaluate(optimal));
    if (!std::isfinite(best)) throw std::invalid_argument("QEM edge error is not finite.");
    return std::max(0.0, best);
}

QemModel makeQemModel(const MeshSource& mesh)
{
    if (mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
        mesh.indices.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("QEM grouping requires a nonempty 32-bit triangle index list.");
    }
    if (mesh.positionIds.size() != mesh.vertices.size()) {
        throw std::invalid_argument("QEM grouping requires a position ID for each vertex.");
    }

    QemModel model;
    model.triangles.reserve(mesh.indices.size() / 3);
    model.vertexPositionIds = mesh.positionIds;
    std::unordered_map<uint32_t, glm::dvec3> positions;
    std::unordered_map<uint32_t, Quadric> quadrics;
    positions.reserve(mesh.vertices.size());
    quadrics.reserve(mesh.vertices.size());
    for (size_t vertex = 0; vertex < mesh.vertices.size(); ++vertex) {
        const auto& p = mesh.vertices[vertex].position;
        const glm::dvec3 position{p.x, p.y, p.z};
        if (!finite(position)) throw std::invalid_argument("QEM grouping requires finite positions.");
        const uint32_t id = mesh.positionIds[vertex];
        const auto [entry, inserted] = positions.try_emplace(id, position);
        if (!inserted && !samePosition(entry->second, position)) {
            throw std::invalid_argument("A QEM position ID refers to multiple positions.");
        }
        quadrics.try_emplace(id);
    }

    model.edges.reserve(mesh.indices.size());
    for (uint32_t triangle = 0; triangle < mesh.indices.size() / 3; ++triangle) {
        const size_t first = static_cast<size_t>(triangle) * 3;
        Triangle ids{};
        for (size_t corner = 0; corner < 3; ++corner) {
            const uint32_t index = mesh.indices[first + corner];
            if (index >= mesh.vertices.size()) throw std::invalid_argument("QEM mesh index is out of range.");
            ids[corner] = mesh.positionIds[index];
        }
        model.triangles.push_back(ids);

        const auto& a = positions.at(ids[0]);
        const auto& b = positions.at(ids[1]);
        const auto& c = positions.at(ids[2]);
        const glm::dvec3 normalUnnormalized = glm::cross(b - a, c - a);
        const double normalLength = glm::length(normalUnnormalized);
        if (normalLength > 0.0 && std::isfinite(normalLength)) {
            const glm::dvec3 normal = normalUnnormalized / normalLength;
            const double d = -glm::dot(normal, a);
            for (uint32_t id : ids) quadrics.at(id).addPlane(normal.x, normal.y, normal.z, d);
        }

        for (size_t side = 0; side < 3; ++side) {
            const uint32_t u = ids[side], v = ids[(side + 1) % 3];
            if (u == v) continue;
            auto& adjacent = model.edges[edgeKey(u, v)].triangles;
            if (std::find(adjacent.begin(), adjacent.end(), triangle) == adjacent.end()) {
                adjacent.push_back(triangle);
            }
        }
    }

    for (auto& entry : model.edges) {
        const uint32_t a = static_cast<uint32_t>(entry.first >> 32);
        const uint32_t b = static_cast<uint32_t>(entry.first);
        entry.second.error = edgeError(quadrics.at(a), quadrics.at(b), positions.at(a), positions.at(b));
    }
    return model;
}

QemClusterErrorStats measureError(const QemModel& model, const ClusterDataset& dataset)
{
    if (dataset.indices.empty() || dataset.indices.size() % 3 != 0 ||
        dataset.indices.size() > std::numeric_limits<uint32_t>::max() || dataset.clusters.empty() ||
        dataset.indices.size() != model.triangles.size() * 3) {
        throw std::invalid_argument("Cannot measure QEM error for an invalid cluster dataset.");
    }
    std::unordered_map<Triangle, std::vector<uint32_t>, TriangleKeyHash> sourceTriangles;
    sourceTriangles.reserve(model.triangles.size());
    for (uint32_t triangle = 0; triangle < model.triangles.size(); ++triangle) {
        sourceTriangles[canonicalTriangle(model.triangles[triangle])].push_back(triangle);
    }

    std::unordered_map<Triangle, size_t, TriangleKeyHash> usedOccurrences;
    std::vector<uint32_t> clusterForTriangle(model.triangles.size(), std::numeric_limits<uint32_t>::max());
    uint64_t nextIndex = 0;
    for (uint32_t clusterId = 0; clusterId < dataset.clusters.size(); ++clusterId) {
        const auto& cluster = dataset.clusters[clusterId];
        if (cluster.firstIndex != nextIndex || cluster.indexCount == 0 || cluster.indexCount % 3 != 0 ||
            nextIndex + cluster.indexCount > dataset.indices.size()) {
            throw std::invalid_argument("Cannot measure QEM error for noncontiguous cluster ranges.");
        }
        for (uint64_t offset = nextIndex; offset < nextIndex + cluster.indexCount; offset += 3) {
            Triangle ids{};
            for (size_t corner = 0; corner < 3; ++corner) {
                const uint32_t index = dataset.indices[static_cast<size_t>(offset + corner)];
                if (index >= model.vertexPositionIds.size()) {
                    throw std::invalid_argument("Cluster dataset index is out of range.");
                }
                ids[corner] = model.vertexPositionIds[index];
            }
            const auto key = canonicalTriangle(ids);
            auto source = sourceTriangles.find(key);
            if (source == sourceTriangles.end()) throw std::invalid_argument("Cluster dataset changed source triangles.");
            const size_t occurrence = usedOccurrences[key]++;
            if (occurrence >= source->second.size()) {
                throw std::invalid_argument("Cluster dataset contains an extra source triangle.");
            }
            clusterForTriangle[source->second[occurrence]] = clusterId;
        }
        nextIndex += cluster.indexCount;
    }
    if (nextIndex != dataset.indices.size()) {
        throw std::invalid_argument("Cluster ranges do not cover the dataset.");
    }

    double errorSum = 0.0;
    size_t adjacencyCount = 0;
    for (const auto& edge : model.edges) {
        const auto& adjacent = edge.second.triangles;
        for (size_t i = 0; i < adjacent.size(); ++i) {
            for (size_t j = i + 1; j < adjacent.size(); ++j) {
                if (clusterForTriangle[adjacent[i]] == clusterForTriangle[adjacent[j]]) {
                    errorSum += edge.second.error;
                    ++adjacencyCount;
                }
            }
        }
    }
    return {adjacencyCount ? errorSum / static_cast<double>(adjacencyCount) : 0.0, adjacencyCount};
}

struct Candidate {
    double error;
    uint32_t triangle;
    uint64_t edge;
};

struct CandidateLater {
    bool operator()(const Candidate& a, const Candidate& b) const
    {
        if (a.error != b.error) return a.error > b.error;
        if (a.triangle != b.triangle) return a.triangle > b.triangle;
        return a.edge > b.edge;
    }
};

} // namespace

QemClusterBuildResult buildQemGuidedClusters(const MeshSource& mesh, uint32_t maxTrianglesPerCluster)
{
    if (maxTrianglesPerCluster == 0) {
        throw std::invalid_argument("A cluster must be allowed at least one triangle.");
    }
    const QemModel model = makeQemModel(mesh);
    const uint32_t triangleCount = static_cast<uint32_t>(model.triangles.size());

    ClusterDataset dataset;
    dataset.indices.reserve(mesh.indices.size());
    std::vector<bool> assigned(triangleCount, false);
    std::vector<uint32_t> group;
    group.reserve(std::min(triangleCount, maxTrianglesPerCluster));
    std::priority_queue<Candidate, std::vector<Candidate>, CandidateLater> frontier;

    for (uint32_t seed = 0; seed < triangleCount; ++seed) {
        if (assigned[seed]) continue;
        while (!frontier.empty()) frontier.pop();
        group.clear();
        group.push_back(seed);
        assigned[seed] = true;

        const auto addFrontier = [&](uint32_t triangle) {
            for (size_t side = 0; side < 3; ++side) {
                const uint32_t a = model.triangles[triangle][side];
                const uint32_t b = model.triangles[triangle][(side + 1) % 3];
                if (a == b) continue;
                const uint64_t key = edgeKey(a, b);
                const auto& edge = model.edges.at(key);
                for (uint32_t neighbor : edge.triangles) {
                    if (!assigned[neighbor]) frontier.push({edge.error, neighbor, key});
                }
            }
        };
        addFrontier(seed);

        while (group.size() < maxTrianglesPerCluster && !frontier.empty()) {
            const Candidate candidate = frontier.top();
            frontier.pop();
            if (assigned[candidate.triangle]) continue;
            assigned[candidate.triangle] = true;
            group.push_back(candidate.triangle);
            addFrontier(candidate.triangle);
        }

        const uint32_t firstIndex = static_cast<uint32_t>(dataset.indices.size());
        const size_t firstVertex = static_cast<size_t>(group.front()) * 3;
        glm::vec3 minimum = mesh.vertices[mesh.indices[firstVertex]].position;
        glm::vec3 maximum = minimum;
        for (uint32_t triangle : group) {
            const size_t first = static_cast<size_t>(triangle) * 3;
            for (size_t corner = 0; corner < 3; ++corner) {
                const uint32_t index = mesh.indices[first + corner];
                dataset.indices.push_back(index);
                minimum = glm::min(minimum, mesh.vertices[index].position);
                maximum = glm::max(maximum, mesh.vertices[index].position);
            }
        }
        dataset.clusters.push_back({firstIndex, static_cast<uint32_t>(group.size() * 3), minimum, maximum});
    }

    const auto errorStats = measureError(model, dataset);
    return {std::move(dataset), errorStats};
}

QemClusterErrorStats measureQemInternalEdgeError(const MeshSource& mesh, const ClusterDataset& dataset)
{
    return measureError(makeQemModel(mesh), dataset);
}
