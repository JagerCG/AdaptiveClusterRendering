#include "SceneData.h"
#include "ClusterGpu.h"
#include "QemSimplifier.h"
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {
using Clock = std::chrono::steady_clock;
using Groups = std::vector<std::vector<uint32_t>>;
uint32_t count32(size_t n)
{
    if (n > UINT32_MAX) throw std::runtime_error("Hierarchy exceeds 32-bit addressing.");
    return static_cast<uint32_t>(n);
}
uint32_t indexAt(const SceneData& s, uint32_t i)
{
    return i < s.dataset.indices.size() ? s.dataset.indices[i]
                                        : s.hierarchy.coarse.indices[i - s.dataset.indices.size()];
}
template <class F> void visitNodeTriangles(const SceneData& s, uint32_t id, F visit)
{
    const auto& node = s.hierarchy.nodes[id];
    for (uint32_t c = 0; c < node.clusterCount; ++c) {
        const auto& cluster = sceneCluster(s, node.firstCluster + c);
        for (uint32_t i = 0; i < cluster.indexCount; i += 3) {
            const auto first = cluster.firstIndex + i;
            visit(indexAt(s, first), indexAt(s, first + 1), indexAt(s, first + 2));
        }
    }
}
Groups groupNeighbors(const SceneData& scene, const LodLevel& level)
{
    std::vector<std::vector<uint32_t>> adjacent(level.nodeCount);
    std::unordered_map<uint64_t, uint32_t> owner;
    for (uint32_t n = 0; n < level.nodeCount; ++n) {
        visitNodeTriangles(scene, level.firstNode + n, [&](uint32_t a, uint32_t b, uint32_t c) {
            const uint32_t p[]{scene.mesh.positionIds[a], scene.mesh.positionIds[b], scene.mesh.positionIds[c]};
            for (unsigned k = 0; k < 3; ++k) {
                const uint32_t x = p[k], y = p[(k + 1) % 3];
                if (x == y) continue;
                const uint64_t key = (uint64_t(std::min(x, y)) << 32) | std::max(x, y);
                auto [it, fresh] = owner.emplace(key, n);
                if (!fresh && it->second != n) {
                    adjacent[n].push_back(it->second);
                    adjacent[it->second].push_back(n);
                }
            }
        });
    }
    // Start with peripheral nodes, then grow across the most shared edges.
    std::vector<uint32_t> order(level.nodeCount), degree(level.nodeCount);
    std::iota(order.begin(), order.end(), 0u);
    for (auto node : order) {
        auto& neighbors = adjacent[node];
        std::sort(neighbors.begin(), neighbors.end());
        for (size_t i = 0; i < neighbors.size(); ++i)
            if (i == 0 || neighbors[i - 1] != neighbors[i]) ++degree[node];
    }
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return degree[a] < degree[b]; });
    Groups groups;
    std::vector<uint32_t> membership(level.nodeCount, kNoParent);
    for (auto seed : order) {
        if (membership[seed] != kNoParent) continue;
        auto id = count32(groups.size());
        auto& group = groups.emplace_back();
        group.push_back(seed);
        membership[seed] = id;
        std::unordered_map<uint32_t, uint32_t> frontier;
        while (group.size() < 4) {
            for (auto other : adjacent[group.back()])
                if (membership[other] == kNoParent) ++frontier[other];
            if (frontier.empty()) break;
            const auto best = std::max_element(frontier.begin(), frontier.end(), [](const auto& a, const auto& b) {
                return a.second == b.second ? a.first > b.first : a.second < b.second;
            });
            group.push_back(best->first);
            membership[best->first] = id;
            frontier.erase(best);
        }
    }
    // A leftover one-node group can share a neighboring parent, up to six
    // children. Disconnected components always retain separate roots.
    for (uint32_t id = 0; id < groups.size(); ++id) {
        if (groups[id].size() != 1) continue;
        const uint32_t node = groups[id].front();
        std::unordered_map<uint32_t, uint32_t> contacts;
        uint32_t best = kNoParent, score = 0;
        for (auto neighbor : adjacent[node]) {
            const auto candidate = membership[neighbor];
            if (candidate == id || groups[candidate].size() >= 6) continue;
            const auto weight = ++contacts[candidate];
            if (weight > score || (weight == score && candidate < best)) {
                best = candidate;
                score = weight;
            }
        }
        if (best != kNoParent) {
            groups[best].push_back(node);
            groups[id].clear();
            membership[node] = best;
        }
    }
    groups.erase(std::remove_if(groups.begin(), groups.end(), [](const auto& g) { return g.empty(); }), groups.end());
    for (auto& group : groups)
        for (auto& id : group)
            id += level.firstNode;
    return groups;
}

std::unordered_set<uint32_t> groupLocks(const SceneData& scene, const Groups& groups,
                                        const std::unordered_set<uint32_t>& seams)
{
    auto locked = seams;
    std::unordered_map<uint32_t, uint32_t> owner;
    for (uint32_t group = 0; group < groups.size(); ++group) {
        for (auto child : groups[group])
            visitNodeTriangles(scene, child, [&](uint32_t a, uint32_t b, uint32_t c) {
                for (auto v : {a, b, c}) {
                    auto pos = scene.mesh.positionIds[v];
                    auto [it, fresh] = owner.emplace(pos, group);
                    if (!fresh && it->second != group) locked.insert(pos);
                }
            });
    }
    return locked;
}

void appendParent(SceneData& scene, const std::vector<uint32_t>& children, const std::unordered_set<uint32_t>& locked,
                  const std::vector<uint32_t>& wedge, unsigned distantGrid)
{
    auto& h = scene.hierarchy;
    MeshSource patch;
    std::unordered_map<uint32_t, uint32_t> local;
    std::vector<uint32_t> originals;
    const auto& first = h.nodes[children.front()];
    LodNode parent{first.boundsMin,
                   0,
                   first.boundsMax,
                   kNoParent,
                   count32(scene.dataset.clusters.size() + h.coarse.clusters.size()),
                   0,
                   count32(h.children.size()),
                   count32(children.size())};
    for (auto id : children) {
        const auto& child = h.nodes[id];
        parent.boundsMin = glm::min(parent.boundsMin, child.boundsMin);
        parent.boundsMax = glm::max(parent.boundsMax, child.boundsMax);
        parent.error = std::max(parent.error, child.error);
        visitNodeTriangles(scene, id, [&](uint32_t a, uint32_t b, uint32_t c) {
            for (auto v : {a, b, c}) {
                auto [it, fresh] = local.emplace(wedge[v], count32(originals.size()));
                if (fresh) {
                    originals.push_back(v);
                    patch.vertices.push_back(scene.mesh.vertices[v]);
                    patch.positionIds.push_back(scene.mesh.positionIds[v]);
                }
                patch.indices.push_back(it->second);
            }
        });
    }
    const double diameter = glm::distance(glm::dvec3(parent.boundsMin), glm::dvec3(parent.boundsMax));
    // Limit an individual collapse to 0.25% of this patch's diagonal at L1, doubling
    // the tolerance at each coarser level. GPU selection still uses measured vertex travel.
    constexpr double firstLevelPlaneFraction = .0025;
    const auto reductionLevel = static_cast<int>(h.levels.size()) - 1;
    const double maxPlaneDistance = std::ldexp(firstLevelPlaneFraction * diameter, reductionLevel);
    const double bound = parent.error + (distantGrid ? simplifyDistantPatch(patch, distantGrid)
                                                     : simplifyPatch(patch, locked, maxPlaneDistance));
    parent.error = static_cast<float>(bound);
    if (parent.error < bound) parent.error = std::nextafter(parent.error, std::numeric_limits<float>::infinity());
    auto rebuilt = buildClusters(patch, scene.maxTrianglesPerCluster);
    const uint32_t firstIndex = count32(scene.dataset.indices.size() + h.coarse.indices.size());
    count32(size_t(firstIndex) + rebuilt.indices.size());
    for (auto v : rebuilt.indices)
        h.coarse.indices.push_back(originals[v]);
    parent.clusterCount = count32(rebuilt.clusters.size());
    for (auto cluster : rebuilt.clusters) {
        cluster.firstIndex += firstIndex;
        h.coarse.clusters.push_back(cluster);
    }
    const auto parentId = count32(h.nodes.size());
    for (auto child : children) {
        h.nodes[child].parent = parentId;
        h.children.push_back(child);
    }
    h.nodes.push_back(parent);
}
} // namespace

LodLevel sceneLevel(const SceneData& scene, uint32_t level)
{
    if (!scene.hierarchy.levels.empty()) return scene.hierarchy.levels.at(level);
    if (level) throw std::out_of_range("Scene has only its original level.");
    return {0, 0, 0, count32(scene.dataset.clusters.size()), 0, count32(scene.dataset.indices.size()), 0};
}

const Cluster& sceneCluster(const SceneData& scene, uint32_t id)
{
    return id < scene.dataset.clusters.size()
               ? scene.dataset.clusters[id]
               : scene.hierarchy.coarse.clusters.at(id - scene.dataset.clusters.size());
}

void buildMeshHierarchy(SceneData& scene, float fineBuildMs)
{
    auto& h = scene.hierarchy;
    h = {};
    LodLevel fine = sceneLevel(scene, 0);
    fine.nodeCount = fine.clusterCount;
    fine.buildMs = fineBuildMs;
    h.levels.push_back(fine);
    for (uint32_t i = 0; i < fine.clusterCount; ++i) {
        const auto& c = scene.dataset.clusters[i];
        h.nodes.push_back({c.boundsMin, 0, c.boundsMax, kNoParent, i, 1, 0, 0});
    }
    // OBJ files without explicit normals can repeat identical corners once per face.
    // Weld attribute-identical copies only in the coarse patch. True UV/normal splits stay locked.
    std::unordered_set<uint32_t> seams;
    std::unordered_map<uint32_t, uint32_t> firstVertex;
    std::vector<uint32_t> wedge(scene.mesh.vertices.size());
    for (uint32_t i = 0; i < wedge.size(); ++i) {
        const auto p = scene.mesh.positionIds[i];
        auto [it, fresh] = firstVertex.emplace(p, i);
        wedge[i] = it->second;
        if (!fresh) {
            const auto& a = scene.mesh.vertices[i];
            const auto& b = scene.mesh.vertices[it->second];
            if (a.position != b.position || a.normal != b.normal || a.texcoord != b.texcoord) seams.insert(p);
        }
    }
    for (uint32_t i = 0; i < wedge.size(); ++i)
        if (seams.count(scene.mesh.positionIds[i])) wedge[i] = i;
    const auto appendLevel = [&](const Groups& groups, unsigned distantGrid, uint32_t maxDiameterPixels,
                                 Clock::time_point start) {
        const auto previous = h.levels.back();
        const auto locked = distantGrid ? std::unordered_set<uint32_t>{} : groupLocks(scene, groups, seams);
        const auto oldChildren = h.children.size();
        const auto oldIndices = h.coarse.indices.size(), oldClusters = h.coarse.clusters.size();
        LodLevel next{count32(h.nodes.size()),
                      count32(groups.size()),
                      count32(scene.dataset.clusters.size() + oldClusters),
                      0,
                      count32(scene.dataset.indices.size() + oldIndices),
                      0,
                      0, maxDiameterPixels};
        for (const auto& group : groups)
            appendParent(scene, group, locked, wedge, distantGrid);
        next.indexCount = count32(h.coarse.indices.size() - oldIndices);
        next.clusterCount = count32(h.coarse.clusters.size() - oldClusters);
        const auto geometryBytes = [](const LodLevel& level) {
            return uint64_t(level.indexCount) * sizeof(uint32_t) +
                   uint64_t(level.clusterCount) * sizeof(GpuClusterDesc);
        };
        if (next.nodeCount == previous.nodeCount &&
            (next.indexCount == previous.indexCount || geometryBytes(next) >= geometryBytes(previous))) {
            h.coarse.indices.resize(oldIndices);
            h.coarse.clusters.resize(oldClusters);
            h.children.resize(oldChildren);
            h.nodes.resize(next.firstNode);
            for (uint32_t n = previous.firstNode; n < next.firstNode; ++n)
                h.nodes[n].parent = kNoParent;
            return false;
        }
        next.buildMs = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
        h.levels.push_back(next);
        return true;
    };
    while (h.levels.size() < 12) {
        const auto start = Clock::now();
        const auto previous = h.levels.back();
        const auto groups = groupNeighbors(scene, previous);
        if (h.levels.size() > 1 && groups.size() == previous.nodeCount && previous.clusterCount == previous.nodeCount)
            break; // one cluster per remaining connected region
        if (!appendLevel(groups, 0, 0, start)) break;
    }
    // These gates limit the longer projected side of the full mesh's bounds.
    for (const auto [cells, pixels] : {std::pair{4u, 14u}, std::pair{2u, 6u}}) {
        const auto start = Clock::now();
        const auto previous = h.levels.back();
        if (previous.indexCount <= 16 * 3) break;
        // Separate regular roots remain separate, even if their bounds overlap.
        Groups roots(previous.nodeCount);
        for (uint32_t i = 0; i < previous.nodeCount; ++i) roots[i].push_back(previous.firstNode + i);
        appendLevel(roots, cells, pixels, start);
    }
}

void validateMeshHierarchy(const SceneData& s)
{
    const auto& h = s.hierarchy;
    const auto check = [](bool ok) {
        if (!ok) throw std::runtime_error("Invalid ACR hierarchy geometry, bounds or links.");
    };
    if (h.levels.empty()) {
        check(h.nodes.empty() && h.children.empty() && h.coarse.indices.empty() && h.coarse.clusters.empty());
        return;
    }
    const uint64_t clusters = s.dataset.clusters.size() + h.coarse.clusters.size();
    const uint64_t indices = s.dataset.indices.size() + h.coarse.indices.size();
    check(indices <= UINT32_MAX && clusters <= UINT32_MAX && h.nodes.size() <= UINT32_MAX &&
          h.children.size() <= UINT32_MAX);
    check(h.levels.front().clusterCount == s.dataset.clusters.size() &&
          h.levels.front().indexCount == s.dataset.indices.size());
    for (auto v : h.coarse.indices)
        check(v < s.mesh.vertices.size());
    uint64_t ni = 0, ci = 0, ii = 0, links = 0;
    std::vector<bool> referenced(h.nodes.size());
    std::vector<uint32_t> childVertexOwner(s.mesh.vertices.size(), kNoParent);
    for (size_t depth = 0; depth < h.levels.size(); ++depth) {
        const auto& level = h.levels[depth];
        check(level.firstNode == ni && level.firstCluster == ci && level.firstIndex == ii && level.nodeCount > 0 &&
              ni + level.nodeCount <= h.nodes.size() && ci + level.clusterCount <= clusters &&
              ii + level.indexCount <= indices && std::isfinite(level.buildMs) &&
              level.buildMs >= 0);
        const auto previousLimit = depth ? h.levels[depth - 1].maxDiameterPixels : 0u;
        check((depth || !level.maxDiameterPixels) &&
              (!previousLimit || (level.maxDiameterPixels && level.maxDiameterPixels < previousLimit)));
        if (depth) check(level.indexCount <= h.levels[depth - 1].indexCount);
        for (uint32_t n = 0; n < level.nodeCount; ++n, ++ni) {
            const auto& node = h.nodes[ni];
            check(node.firstCluster == ci && node.clusterCount && ci + node.clusterCount <= clusters &&
                  node.firstChild == links && links + node.childCount <= h.children.size() &&
                  std::isfinite(node.error) && node.error >= 0 &&
                (depth ? node.childCount > 0 : node.childCount == 0 && node.error == 0 && node.clusterCount == 1) &&
                (!level.maxDiameterPixels || node.childCount == 1));
            check((depth + 1 == h.levels.size()) == (node.parent == kNoParent));
            for (int axis = 0; axis < 3; ++axis)
                check(std::isfinite(node.boundsMin[axis]) && std::isfinite(node.boundsMax[axis]) &&
                      node.boundsMin[axis] <= node.boundsMax[axis]);
            for (uint32_t k = 0; k < node.childCount; ++k, ++links) {
                uint32_t child = h.children[links];
                check(child >= h.levels[depth - 1].firstNode && child < level.firstNode && !referenced[child]);
                referenced[child] = true;
                const auto& c = h.nodes[child];
                check(c.parent == ni && c.error <= node.error);
                visitNodeTriangles(s, child, [&](auto a, auto b, auto cIndex) {
                    childVertexOwner[a] = childVertexOwner[b] = childVertexOwner[cIndex] = static_cast<uint32_t>(ni);
                });
                for (int axis = 0; axis < 3; ++axis)
                    check(node.boundsMin[axis] <= c.boundsMin[axis] && node.boundsMax[axis] >= c.boundsMax[axis]);
            }
            for (uint32_t k = 0; k < node.clusterCount; ++k, ++ci) {
                const auto& c = sceneCluster(s, static_cast<uint32_t>(ci));
                check(c.firstIndex == ii && c.indexCount && c.indexCount % 3 == 0 &&
                      c.indexCount / 3 <= s.maxTrianglesPerCluster && ii + c.indexCount <= indices);
                for (int axis = 0; axis < 3; ++axis)
                    check(c.boundsMin[axis] >= node.boundsMin[axis] && c.boundsMax[axis] <= node.boundsMax[axis] &&
                          c.boundsMin[axis] <= c.boundsMax[axis]);
                if (depth)
                    for (uint32_t j = 0; j < c.indexCount; ++j) {
                        const auto vertex = indexAt(s, c.firstIndex + j);
                        check(childVertexOwner[vertex] == ni);
                        const auto& p = s.mesh.vertices[vertex].position;
                        for (int axis = 0; axis < 3; ++axis)
                            check(p[axis] >= c.boundsMin[axis] && p[axis] <= c.boundsMax[axis]);
                    }
                ii += c.indexCount;
            }
        }
        check(ci == uint64_t(level.firstCluster) + level.clusterCount &&
              ii == uint64_t(level.firstIndex) + level.indexCount);
    }
    check(ni == h.nodes.size() && ci == clusters && ii == indices && links == h.children.size());
    for (size_t i = 0; i < h.levels.back().firstNode; ++i)
        check(referenced[i]);
}
