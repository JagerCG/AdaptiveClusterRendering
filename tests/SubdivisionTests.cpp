#include "GeometryGenerationPlan.h"
#include "GeometryValidation.h"
#include "TessellationFixtures.h"

#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace {
void expect(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
void rejected(const std::function<void()>& action)
{
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    expect(failed, "Invalid subdivision request / result was accepted.");
}
void checkPlans()
{
    for (auto fixture : {TessellationFixture::Triangle, TessellationFixture::Plane, TessellationFixture::ClusterGrid}) {
        const auto source = makeTessellationFixture(fixture);
        const auto count = static_cast<uint32_t>(source.dataset.indices.size());
        const auto clusters = static_cast<uint32_t>(source.dataset.clusters.size());
        for (uint32_t n : {1u, 2u, 4u, 8u}) {
            for (const auto request : {ClusterSelection{9, ClusterSelectionMode::All, {}},
                                      ClusterSelection{9, ClusterSelectionMode::Explicit, {clusters - 1}},
                                      ClusterSelection{9, ClusterSelectionMode::Explicit, {}}}) {
                const auto selection = resolveClusterSelection(source.dataset.clusters, count, 9, request);
                const auto plan = makeGeometryGenerationPlan(count, clusters, selection, n);
                expect(plan.edgeSegments == n && plan.sourceTriangleCount == selection.triangleCount(), "Plan lost source counts / settings.");
                expect(plan.indexCount == selection.indexCount * n * n && plan.vertexCount == plan.indexCount,
                       "Expected T*n*n triangles with three unshared vertices each.");
                expect(plan.vertexBytes == uint64_t(plan.vertexCount) * 32 && plan.indexBytes == uint64_t(plan.indexCount) * 4 &&
                       plan.worklistBytes == selection.triangleCount() * 4 && plan.triangleFirstIndices.size() == selection.triangleCount(),
                       "Output scales with n squared; the source work list must not scale.");
                uint32_t first = 0;
                for (size_t i = 0; i < plan.ranges.size(); ++i) {
                    expect(plan.ranges[i].sourceClusterId == selection.ranges[i].sourceClusterId &&
                           plan.ranges[i].firstIndex == first && plan.ranges[i].indexCount == selection.ranges[i].indexCount * n * n,
                           "Subdivision broke cluster mapping / output ranges.");
                    first += plan.ranges[i].indexCount;
                }
            }
        }
    }
    ResolvedClusterSelection one{1, true, {{0, 0, 3}}, 3};
    for (uint32_t n : {0u, 3u, 5u, 16u, std::numeric_limits<uint32_t>::max()}) {
        rejected([&] { makeGeometryGenerationPlan(3, 1, one, n); });
    }
    expect(makeGeometryGenerationPlan(3, 1, one, 8, {6912, 6144, 1}).indexCount == 192, "n=8 exact budget should fit.");
    rejected([&] { makeGeometryGenerationPlan(3, 1, one, 8, {6911, 6144, 1}); });
    rejected([&] { makeGeometryGenerationPlan(3, 1, one, 8, {6912, 6143, 1}); });
    ResolvedClusterSelection two{1, true, {{0, 0, 6}}, 6};
    rejected([&] { makeGeometryGenerationPlan(6, 1, two, 8, {14000, 14000, 1}); });
    ResolvedClusterSelection overflow{1, true, {{0, 0, 100000002}}, 100000002};
    rejected([&] { makeGeometryGenerationPlan(100000002, 1, overflow, 8,
        {std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint32_t>::max()}); });
    ResolvedClusterSelection reversed{1, false, {{3, 18, 6}, {0, 0, 6}}, 12};
    const auto plan = makeGeometryGenerationPlan(24, 4, reversed, 4);
    expect(plan.ranges[0].sourceClusterId == 3 && plan.ranges[0].indexCount == 96 &&
           plan.ranges[1].sourceClusterId == 0 && plan.ranges[1].firstIndex == 96 &&
           plan.triangleFirstIndices == std::vector<uint32_t>({18, 21, 0, 3}), "Reversed n=4 selection was not preserved.");
}

void checkVerifier()
{
    auto scene = makeTessellationFixture(TessellationFixture::Triangle);
    // Vary normals too: midpoint normals must remain linear, not normalized.
    scene.mesh.vertices[0].normal = {1, 0, 0};
    scene.mesh.vertices[1].normal = {0, 1, 0};
    scene.mesh.vertices[2].normal = {0, 0, 1};
    const auto selection = resolveClusterSelection(scene.dataset.clusters, 3, 1, {1, ClusterSelectionMode::All, {}});
    const auto& a = scene.mesh.vertices[0];
    const auto& b = scene.mesh.vertices[1];
    const auto& c = scene.mesh.vertices[2];
    const Vertex ab{{0, -1, 0}, {.5f, .5f, 0}, {.5f, 0}};
    const Vertex ac{{-1, 0, 0}, {.5f, 0, .5f}, {0, .5f}};
    const Vertex bc{{0, 0, 0}, {0, .5f, .5f}, {.5f, .5f}};
    const std::vector<Vertex> good{a, ab, ac, ab, bc, ac, ab, b, bc, ac, bc, c};
    std::vector<uint32_t> indices(good.size());
    std::iota(indices.begin(), indices.end(), 0);
    const auto report = validateGeneratedGeometry(scene, selection, 2, good, indices);
    expect(report.triangles == 4 && report.sharedEdgeChecks > 0 && report.maxAttributeError == 0,
           "Hand-authored n=2 geometry did not pass the independent verifier.");
    validateGeneratedGeometry(scene, selection, 1, {a, b, c}, {0, 1, 2});
    auto bad = good;
    bad[1].texcoord.x = 0.25f;
    rejected([&] { validateGeneratedGeometry(scene, selection, 2, bad, indices); });
    bad = good;
    bad[1].normal = {.7071f, .7071f, 0};
    rejected([&] { validateGeneratedGeometry(scene, selection, 2, bad, indices); });
    bad = good;
    bad[1].position.z = .01f;
    rejected([&] { validateGeneratedGeometry(scene, selection, 2, bad, indices); });
    bad = good;
    std::swap(bad[1], bad[2]);
    rejected([&] { validateGeneratedGeometry(scene, selection, 2, bad, indices); });
    bad = good;
    bad[3].position.x += 1e-7f; // within interpolation tolerance, but breaks exact shared-edge equality
    rejected([&] { validateGeneratedGeometry(scene, selection, 2, bad, indices); });
    bad = good;
    bad.pop_back();
    rejected([&] { validateGeneratedGeometry(scene, selection, 2, bad, indices); });
    indices.back() = 999;
    rejected([&] { validateGeneratedGeometry(scene, selection, 2, good, indices); });
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) throw std::runtime_error("Expected plans or verifier.");
        const std::string name = argv[1];
        if (name == "plans") checkPlans();
        else if (name == "verifier") checkVerifier();
        else throw std::runtime_error("Unknown subdivision test.");
        std::cout << "Subdivision " << name << " passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
