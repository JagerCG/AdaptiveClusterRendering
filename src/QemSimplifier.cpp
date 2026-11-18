#include "QemSimplifier.h"
#include "Quadric.h"
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <tuple>

namespace {
struct Edge {
    uint32_t lo, hi;
    bool forward;
};
struct Candidate {
    double cost, length;
    uint32_t from, to;
};
using Ring = std::vector<std::vector<uint32_t>>;

bool legalCollapse(const MeshSource& mesh, const Ring& faces, const Ring& neighbors, uint32_t from, uint32_t to,
                   double maxPlaneDistance)
{
    // The link of an interior manifold edge consists of its two opposite
    // vertices. Additional shared neighbors would pinch the surface.
    unsigned shared = 0;
    for (uint32_t v : neighbors[from])
        if (std::binary_search(neighbors[to].begin(), neighbors[to].end(), v)) ++shared;
    if (shared != 2) return false;
    for (uint32_t first : faces[from]) {
        const auto* tri = mesh.indices.data() + first;
        unsigned corner = tri[0] == from ? 0 : tri[1] == from ? 1 : 2;
        const uint32_t a = tri[(corner + 1) % 3], b = tri[(corner + 2) % 3];
        if (a == to || b == to) continue; // the two incident faces disappear
        for (uint32_t other : faces[to]) {
            const auto* t = mesh.indices.data() + other;
            if (std::find(t, t + 3, a) != t + 3 && std::find(t, t + 3, b) != t + 3)
                return false; // duplicate face, e.g. collapsing a tetrahedron
        }
        const glm::dvec3 p = mesh.vertices[a].position, edge = glm::dvec3(mesh.vertices[b].position) - p;
        const auto before = glm::cross(edge, glm::dvec3(mesh.vertices[from].position) - p);
        const auto after = glm::cross(edge, glm::dvec3(mesh.vertices[to].position) - p);
        const double oldArea = glm::length(before), newArea = glm::length(after);
        if (glm::dot(before, after) <= .5 * oldArea * newArea) return false;
        // Each old incident triangle contains the removed point. A replacement face
        // cutting too far past it makes a visible dent, even if its winding survives.
        if (std::abs(glm::dot(after, glm::dvec3(mesh.vertices[from].position) - p)) > maxPlaneDistance * newArea)
            return false;
        const auto shape = [&](uint32_t vertex, double area) {
            const auto side = glm::dvec3(mesh.vertices[vertex].position) - p;
            return area / (glm::dot(edge, edge) + glm::dot(side, side) + glm::dot(side - edge, side - edge));
        };
        const double quality = shape(to, newArea);
        if (quality < .06 && quality < shape(from, oldArea)) return false;
    }
    return true;
}
} // namespace

double simplifyPatch(MeshSource& mesh, const std::unordered_set<uint32_t>& lockedPositions, double maxPlaneDistance)
{
    if (mesh.indices.size() <= 3) return 0.0;
    const size_t target = std::max<size_t>(1, mesh.indices.size() / 6);
    const size_t count = mesh.vertices.size();
    std::vector<Quadric> quadrics(count);
    std::vector<uint32_t> representative(count);
    std::iota(representative.begin(), representative.end(), 0u);
    std::vector<bool> locked(count);
    for (size_t i = 0; i < count; ++i)
        locked[i] = lockedPositions.count(mesh.positionIds[i]) != 0;
    // Keep support points along the axes, face diagonals and body diagonals.
    const std::array<glm::dvec3, 13> directions{{
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, -1, 0}, {1, 0, 1}, {1, 0, -1},
        {0, 1, 1}, {0, 1, -1}, {1, 1, 1}, {1, 1, -1}, {1, -1, 1}, {-1, 1, 1}}};
    for (const auto& dir : directions) {
        const auto limits =
            std::minmax_element(mesh.vertices.begin(), mesh.vertices.end(), [&](const Vertex& a, const Vertex& b) {
                return glm::dot(glm::dvec3(a.position), dir) < glm::dot(glm::dvec3(b.position), dir);
            });
        locked[limits.first - mesh.vertices.begin()] = true;
        locked[limits.second - mesh.vertices.begin()] = true;
    }
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        const uint32_t a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        const glm::dvec3 p = mesh.vertices[a].position;
        auto n = glm::cross(glm::dvec3(mesh.vertices[b].position) - p, glm::dvec3(mesh.vertices[c].position) - p);
        const double length = glm::length(n);
        if (length == 0) {
            locked[a] = locked[b] = locked[c] = true;
            continue;
        }
        n /= length;
        for (uint32_t v : {a, b, c})
            quadrics[v].addPlane(n.x, n.y, n.z, -glm::dot(n, p));
    }
    while (mesh.indices.size() / 3 >= target + 2) {
        Ring faces(count), neighbors(count);
        std::vector<Edge> edges;
        edges.reserve(mesh.indices.size());
        for (uint32_t i = 0; i < mesh.indices.size(); i += 3) {
            for (unsigned k = 0; k < 3; ++k) {
                const uint32_t a = mesh.indices[i + k], b = mesh.indices[i + (k + 1) % 3];
                faces[a].push_back(i);
                edges.push_back({std::min(a, b), std::max(a, b), a < b});
            }
        }
        std::sort(edges.begin(), edges.end(),
                  [](Edge a, Edge b) { return std::tie(a.lo, a.hi) < std::tie(b.lo, b.hi); });
        std::vector<Candidate> candidates;
        for (size_t i = 0; i < edges.size();) {
            const auto e = edges[i];
            size_t j = i + 1;
            while (j < edges.size() && edges[j].lo == e.lo && edges[j].hi == e.hi)
                ++j;
            neighbors[e.lo].push_back(e.hi);
            neighbors[e.hi].push_back(e.lo);
            if (j - i != 2 || e.forward == edges[j - 1].forward || e.lo == e.hi) {
                locked[e.lo] = locked[e.hi] = true; // open, split or non-manifold edge
            }
            else if (!locked[e.lo] || !locked[e.hi]) {
                auto q = quadrics[e.lo];
                q += quadrics[e.hi];
                const glm::dvec3 a = mesh.vertices[e.lo].position, b = mesh.vertices[e.hi].position;
                const double length = glm::distance(a, b);
                // A small length penalty distinguishes nearly equivalent plane
                // fits and discourages long, thin triangles in smooth regions.
                const double regularizer = .1 * length * length;
                // A protected endpoint can absorb its neighbor while keeping its exact position and attributes.
                if (!locked[e.lo])
                    candidates.push_back({std::max(0.0, q.evaluate(b)) + regularizer, length, e.lo, e.hi});
                if (!locked[e.hi])
                    candidates.push_back({std::max(0.0, q.evaluate(a)) + regularizer, length, e.hi, e.lo});
            }
            i = j;
        }
        for (auto& ring : neighbors)
            std::sort(ring.begin(), ring.end());
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return std::tie(a.cost, a.length, a.from, a.to) < std::tie(b.cost, b.length, b.from, b.to);
        });
        std::vector<bool> busy(count);
        std::vector<uint32_t> remap(count);
        std::iota(remap.begin(), remap.end(), 0u);
        size_t remaining = mesh.indices.size() / 3;
        for (const auto& c : candidates) {
            if (remaining < target + 2) break;
            if (locked[c.from] || busy[c.from] || busy[c.to] ||
                !legalCollapse(mesh, faces, neighbors, c.from, c.to, maxPlaneDistance)) continue;
            remap[c.from] = c.to;
            representative[c.from] = c.to;
            quadrics[c.to] += quadrics[c.from];
            for (auto v : {c.from, c.to}) {
                busy[v] = true;
                for (auto n : neighbors[v])
                    busy[n] = true;
            }
            remaining -= 2;
        }
        if (remaining == mesh.indices.size() / 3) break;
        size_t written = 0;
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            const auto a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
            if ((a != b && remap[a] == remap[b]) || (a != c && remap[a] == remap[c]) ||
                (b != c && remap[b] == remap[c]))
                continue;
            mesh.indices[written++] = remap[a];
            mesh.indices[written++] = remap[b];
            mesh.indices[written++] = remap[c];
        }
        mesh.indices.resize(written);
    }
    // Each child triangle maps linearly onto a parent face, edge or point.
    // Its movement is bounded by the largest original-to-final corner offset.
    double error = 0;
    for (uint32_t v = 0; v < count; ++v) {
        uint32_t root = v;
        while (representative[root] != root)
            root = representative[root];
        for (uint32_t i = v; representative[i] != root;) {
            const auto next = representative[i];
            representative[i] = root;
            i = next;
        }
        error = std::max(
            error, glm::distance(glm::dvec3(mesh.vertices[v].position), glm::dvec3(mesh.vertices[root].position)));
    }
    return error;
}

double simplifyDistantPatch(MeshSource& mesh, unsigned cellsPerAxis)
{
    auto lo = glm::dvec3(mesh.vertices.front().position), hi = lo;
    for (const auto& v : mesh.vertices) {
        lo = glm::min(lo, glm::dvec3(v.position));
        hi = glm::max(hi, glm::dvec3(v.position));
    }
    const glm::dvec3 span = hi - lo;
    const double width = std::max({span.x, span.y, span.z});
    if (width == 0) return 0;
    const glm::dvec3 center = (lo + hi) * .5;
    const size_t cellCount = size_t(cellsPerAxis) * cellsPerAxis * cellsPerAxis;
    std::vector<uint32_t> cell(mesh.vertices.size()), support(cellCount, UINT32_MAX);
    std::vector<double> best(cellCount, -std::numeric_limits<double>::infinity());
    for (uint32_t i = 0; i < mesh.vertices.size(); ++i) {
        const glm::dvec3 offset = (glm::dvec3(mesh.vertices[i].position) - center) / width;
        const auto grid = glm::clamp(glm::ivec3((offset + .5) * double(cellsPerAxis)),
                                     glm::ivec3(0), glm::ivec3(cellsPerAxis - 1));
        const auto id = grid.x + cellsPerAxis * (grid.y + cellsPerAxis * grid.z);
        cell[i] = id;
        // Retain an existing outermost point in the cell's radial direction.
        const double score = glm::dot(offset, glm::dvec3(grid) + .5 - .5 * cellsPerAxis);
        if (score > best[id]) {
            best[id] = score;
            support[id] = i;
        }
    }
    std::vector<uint32_t> indices;
    std::set<std::array<uint32_t, 3>> faces;
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        std::array<uint32_t, 3> tri{};
        for (int k = 0; k < 3; ++k) tri[k] = support[cell[mesh.indices[i + k]]];
        if (tri[0] == tri[1] || tri[1] == tri[2] || tri[2] == tri[0]) continue;
        const auto a = glm::dvec3(mesh.vertices[tri[0]].position);
        const auto normal = glm::cross(glm::dvec3(mesh.vertices[tri[1]].position) - a,
                                       glm::dvec3(mesh.vertices[tri[2]].position) - a);
        if (glm::dot(normal, normal) == 0) continue;
        auto key = tri;
        std::sort(key.begin(), key.end());
        if (faces.insert(key).second) indices.insert(indices.end(), tri.begin(), tri.end());
    }
    // A thin component must remain drawable even when it fits inside just one or two cells.
    if (indices.empty() || indices.size() >= mesh.indices.size()) return 0;
    mesh.indices = std::move(indices);
    // The box diagonal also bounds points on faces lost when the grid removes degenerate triangles.
    return glm::length(span);
}
