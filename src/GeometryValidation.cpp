#include "GeometryValidation.h"
#include "GeometryGenerationPlan.h"
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

namespace {
void require(bool valid, const char* error)
{
    if (!valid) throw std::runtime_error(error);
}
std::array<float, 8> attributes(const Vertex& v)
{
    std::array<float, 8> result;
    static_assert(sizeof(result) == sizeof(Vertex));
    std::memcpy(result.data(), &v, sizeof(v));
    return result;
}
glm::dvec3 unitOrZero(glm::dvec3 v)
{
    const double scale = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
    return scale <= 1e-20 ? glm::dvec3(0) : glm::normalize(v / scale);
}
} // namespace

GeometryValidationStats validateGeneratedGeometry(const SceneData& source,
    const ResolvedClusterSelection& selection, uint32_t n,
    const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
    const HeightField* height, DisplacementSettings displacement, uint32_t geometryLevel)
{
    validateDisplacement(displacement);
    require(!displacement.enabled || height, "Displacement verification requires a height map.");
    const double bound = height ? displacementBound(*height, displacement) : 0;
    const bool displaced = bound > 0;
    require(validEdgeSegments(n), "Invalid subdivision level in geometry verification.");
    require(vertices.size() == selection.indexCount * n * n && indices.size() == vertices.size(),
            "Generated counts do not equal selected source triangles times n squared.");
    GeometryValidationStats stats;
    const bool mixed = geometryLevel == kAdaptiveGeometryLevel;
    const auto level = sceneLevel(source, mixed ? 0 : geometryLevel);
    const size_t clusters = mixed ? source.dataset.clusters.size() + source.hierarchy.coarse.clusters.size() : level.clusterCount;
    const size_t indexCount = mixed ? source.dataset.indices.size() + source.hierarchy.coarse.indices.size() : level.indexCount;
    size_t cursor = 0;
    // Shared source-index edges have a canonical sample number from low to high
    // index. Even across cluster boundaries, matching samples must be bit-identical.
    std::map<std::array<uint32_t, 3>, Vertex> edgeSamples;
    for (const auto& range : selection.ranges) {
        require(range.sourceClusterId < clusters, "Invalid source cluster in verification.");
        const auto& cluster = sceneCluster(source,level.firstCluster+range.sourceClusterId);
        require(uint64_t(range.firstIndex) + range.indexCount <= indexCount && range.indexCount % 3 == 0 &&
                level.firstIndex + range.firstIndex == cluster.firstIndex && range.indexCount == cluster.indexCount,
                "Invalid source range in geometry verification.");
        for (uint32_t first = range.firstIndex; first < range.firstIndex + range.indexCount; first += 3) {
            std::array<uint32_t, 3> ids;
            std::array<Vertex, 3> original;
            std::array<std::array<float, 8>, 3> fields;
            for (uint32_t k = 0; k < 3; ++k) {
                const size_t address = size_t(level.firstIndex) + first + k;
                ids[k] = address < source.dataset.indices.size() ? source.dataset.indices[address] :
                         source.hierarchy.coarse.indices[address - source.dataset.indices.size()];
                require(ids[k] < source.mesh.vertices.size(), "Invalid source vertex in geometry verification.");
                original[k] = source.mesh.vertices[ids[k]];
                fields[k] = attributes(original[k]);
            }
            const auto a = glm::dvec3(original[0].position), b = glm::dvec3(original[1].position), c = glm::dvec3(original[2].position);
            const auto sourceCross = glm::cross(b - a, c - a);
            const double sourceArea = glm::length(sourceCross);
            double areaSum = 0;
            const auto vertex = [&](uint32_t x, uint32_t y) {
                require(cursor < vertices.size(), "Subdivision emitted too few vertices.");
                const auto& actual = vertices[cursor];
                require(indices[cursor] == cursor, "Generated index order / range is incorrect.");
                ++cursor;
                const std::array<uint32_t, 3> w{n - x - y, x, y};
                const auto values = attributes(actual);
                for (uint32_t k = 0; k < 3; ++k) {
                    if (!displaced && w[k] == n) require(std::memcmp(&actual, &original[k], sizeof(Vertex)) == 0,
                                          "An original corner was not preserved bit for bit.");
                }
                std::array<double, 8> expected;
                for (size_t field = 0; field < 8; ++field)
                    expected[field] = (double(fields[0][field]) * w[0] + double(fields[1][field]) * w[1] +
                                       double(fields[2][field]) * w[2]) / n;
                const glm::dvec3 base(expected[0], expected[1], expected[2]);
                const glm::dvec3 normal(expected[3], expected[4], expected[5]);
                if (displaced) {
                    const double distance = displacement.amplitude * (2 * sampleHeight(*height, expected[6], expected[7]) - 1);
                    const auto position = base + unitOrZero(normal) * distance;
                    for (size_t k = 0; k < 3; ++k) expected[k] = position[static_cast<glm::length_t>(k)];
                }
                for (size_t field = 0; field < 8; ++field) {
                    if (displaced && field >= 3 && field < 6) continue; // geometric normals checked per triangle below
                    // Double-precision barycentric reference, independent of shader
                    // row decoding and its canonical floating-point edge formula.
                    const double error = std::abs(double(values[field]) - expected[field]);
                    const double scale = std::max({1.0, std::abs(double(fields[0][field])),
                        std::abs(double(fields[1][field])), std::abs(double(fields[2][field]))});
                    const double samplingError = displaced && field < 3 ?
                        2e-6 * displacement.amplitude * std::max(height->width, height->height) : 0;
                    require(std::isfinite(values[field]) && error <= 2e-6 * scale + samplingError,
                            "Generated position, UV or linear normal differs from the barycentric reference.");
                    stats.maxAttributeError = std::max(stats.maxAttributeError, error);
                }
                for (glm::length_t k = 0; k < 3; ++k) {
                    const double tolerance = 2e-6 * std::max({1.0, std::abs(base[k]), bound});
                    require(actual.position[k] >= double(cluster.boundsMin[k]) - bound - tolerance &&
                            actual.position[k] <= double(cluster.boundsMax[k]) + bound + tolerance,
                            "Generated vertex escapes its conservatively expanded cluster bounds.");
                }
                require(glm::length(glm::dvec3(actual.position) - base) <= bound + 5e-6 * std::max(1.0, glm::length(base)),
                        "Vertex displacement exceeds its conservative bound.");
                for (uint32_t opposite = 0; opposite < 3; ++opposite) {
                    if (w[opposite] != 0) continue;
                    uint32_t lo = (opposite + 1) % 3, hi = (opposite + 2) % 3;
                    if (ids[lo] > ids[hi]) std::swap(lo, hi);
                    const std::array<uint32_t, 3> key{ids[lo], ids[hi], w[hi]};
                    const auto inserted = edgeSamples.emplace(key, actual);
                    if (!inserted.second) {
                        const auto& other = inserted.first->second;
                        const bool equal = displaced ?
                            std::memcmp(&other.position, &actual.position, sizeof(glm::vec3)) == 0 &&
                            std::memcmp(&other.texcoord, &actual.texcoord, sizeof(glm::vec2)) == 0 :
                            std::memcmp(&other, &actual, sizeof(Vertex)) == 0;
                        require(equal,
                                "Matching shared-edge samples differ (possible seam / crack).");
                        ++stats.sharedEdgeChecks;
                    }
                }
                return std::make_pair(glm::dvec3(actual.position), normal);
            };
            const auto triangle = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2) {
                const size_t start = cursor;
                const auto p0 = vertex(x0, y0), p1 = vertex(x1, y1), p2 = vertex(x2, y2);
                const auto cross = glm::cross(p1.first - p0.first, p2.first - p0.first);
                if (!displaced && sourceArea > 1e-20) require(glm::dot(cross, sourceCross) > 0, "Subdivision reversed or collapsed a triangle.");
                if (displaced) {
                    const auto face = unitOrZero(glm::cross(unitOrZero(p1.first - p0.first), unitOrZero(p2.first - p0.first)));
                    const glm::dvec3 linearNormals[] = {p0.second, p1.second, p2.second};
                    for (size_t k = 0; k < 3; ++k) {
                        auto expected = glm::dot(face, face) > 0 ? face : unitOrZero(linearNormals[k]);
                        if (glm::dot(expected, expected) == 0) expected = glm::dvec3(0,0,1);
                        const double error = glm::length(glm::dvec3(vertices[start + k].normal) - expected);
                        require(std::isfinite(error) && error <= 5e-4, "Displaced normal does not match the generated face.");
                        stats.maxNormalError = std::max(stats.maxNormalError, error);
                    }
                }
                areaSum += glm::length(cross);
                ++stats.triangles;
            };
            // Enumerate the triangular lattice directly, not the GPU's linear ID decoder.
            for (uint32_t y = 0; y < n; ++y) {
                for (uint32_t x = 0; x < n - y; ++x) {
                    triangle(x, y, x + 1, y, x, y + 1);
                    if (x + y + 1 < n) triangle(x + 1, y, x + 1, y + 1, x, y + 1);
                }
            }
            const double edgeScale = std::max(glm::dot(b - a, b - a), glm::dot(c - a, c - a));
            require(displaced || std::abs(areaSum - sourceArea) <= 1e-4 * sourceArea + 1e-8 * edgeScale,
                    "Subdivision changed the total area of a source triangle.");
        }
    }
    require(cursor == vertices.size(), "Subdivision emitted extra vertices.");
    return stats;
}
