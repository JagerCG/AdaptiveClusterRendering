#include "SceneData.h"
#include <cmath>
#include <stdexcept>

namespace {
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
} // namespace

AcrLevelRecord sceneLevel(const SceneData& scene, uint32_t level)
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
