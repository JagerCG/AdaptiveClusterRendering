#include "GeometryGenerationPlan.h"
#include "TessellationFixtures.h"

#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
void rejected(const std::function<void()>& action)
{
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    expect(failed, "Invalid / oversized geometry copy plan was accepted.");
}
void checkSelection()
{
    const auto grid = makeTessellationFixture(TessellationFixture::ClusterGrid);
    const auto resolve = [&](ClusterSelectionMode mode, std::vector<uint32_t> ids) {
        return resolveClusterSelection(grid.dataset.clusters, 24, 7, {7, mode, std::move(ids)});
    };
    const auto all = resolve(ClusterSelectionMode::All, {});
    const auto plan = makeGeometryGenerationPlan(24, 4, all);
    expect(plan.vertexCount == 24 && plan.indexCount == 24 && plan.vertexBytes == 768 &&
           plan.indexBytes == 96 && plan.worklistBytes == 32 && plan.workgroupsX == 1,
           "Grid output should have three unshared vertices per triangle.");
    expect(plan.triangleFirstIndices == std::vector<uint32_t>({0, 3, 6, 9, 12, 15, 18, 21}),
           "All selection lost a source triangle.");
    const auto reverse = resolve(ClusterSelectionMode::Explicit, {3, 0});
    const auto compact = makeGeometryGenerationPlan(24, 4, reverse);
    expect(compact.triangleFirstIndices == std::vector<uint32_t>({18, 21, 0, 3}) &&
           compact.vertexCount == 12 && compact.indexCount == 12 && compact.ranges.size() == 2,
           "Partial / reversed input was not compacted in request order.");
    expect(compact.ranges[0].sourceClusterId == 3 && compact.ranges[0].firstIndex == 0 && compact.ranges[0].indexCount == 6 &&
           compact.ranges[1].sourceClusterId == 0 && compact.ranges[1].firstIndex == 6 && compact.ranges[1].indexCount == 6,
           "Compact ranges must retain source IDs while using output offsets.");
    const auto empty = makeGeometryGenerationPlan(24, 4, resolve(ClusterSelectionMode::Explicit, {}));
    expect(empty.triangleFirstIndices.empty() && empty.ranges.empty() && empty.vertexCount == 0 && empty.indexCount == 0 &&
           empty.vertexBytes == 0 && empty.indexBytes == 0 && empty.worklistBytes == 0 && empty.workgroupsX == 0,
           "Explicit empty selection must produce no allocations / dispatch.");
    expect(sameClusterSelection(reverse, reverse), "Equivalent selection must match the cache key.");
    expect(!sameClusterSelection(reverse, resolve(ClusterSelectionMode::Explicit, {0, 3})), "Order is part of the cache key.");
    auto changed = reverse;
    ++changed.sceneRevision;
    expect(!sameClusterSelection(reverse, changed), "Source revision is part of the cache key.");
    changed = reverse;
    ++changed.ranges[0].firstIndex;
    expect(!sameClusterSelection(reverse, changed), "Source range is part of the cache key.");
    expect(!sameClusterSelection(all, resolve(ClusterSelectionMode::Explicit, {0, 1, 2, 3})), "Selection mode is part of the key.");
}
void checkLimits()
{
    ResolvedClusterSelection one{1, true, {{0, 0, 3}}, 3};
    const auto single = makeGeometryGenerationPlan(3, 1, one, 1, {108, 96, 1});
    expect(single.vertexBytes + single.indexBytes == 108, "Exact output payload budget should fit.");
    rejected([&] { makeGeometryGenerationPlan(3, 1, one, 1, {107, 96, 1}); });
    rejected([&] { makeGeometryGenerationPlan(3, 1, one, 1, {108, 95, 1}); });
    rejected([&] { makeGeometryGenerationPlan(3, 1, one, 1, {108, 96, 0}); });
    auto changed = one;
    changed.sceneRevision = 0;
    rejected([&] { makeGeometryGenerationPlan(3, 1, changed); });
    changed = one;
    changed.indexCount = 6;
    rejected([&] { makeGeometryGenerationPlan(3, 1, changed); });
    changed = one;
    changed.ranges[0].sourceClusterId = 1;
    rejected([&] { makeGeometryGenerationPlan(3, 1, changed); });
    changed = one;
    changed.ranges.push_back(changed.ranges.front());
    changed.indexCount = 6;
    rejected([&] { makeGeometryGenerationPlan(3, 1, changed); });
    for (const auto invalidRange : {SelectedClusterRange{0, 1, 3}, {0, 0, 2}, {0, 0, 0}, {0, 3, 3}}) {
        changed = one;
        changed.ranges = {invalidRange};
        changed.indexCount = invalidRange.indexCount;
        rejected([&] { makeGeometryGenerationPlan(3, 1, changed); });
    }
    constexpr uint32_t max = std::numeric_limits<uint32_t>::max();
    ResolvedClusterSelection huge{1, true, {{0, 0, max}}, max};
    rejected([&] { makeGeometryGenerationPlan(max, 1, huge); }); // reject before trying to allocate billions of entries
    huge.ranges.push_back({1, 0, max});
    huge.indexCount = uint64_t(max) * 2;
    rejected([&] { makeGeometryGenerationPlan(max, 2, huge); }); // cannot truncate uint64 count
    huge = {1, true, {{0, max - 3, 6}}, 6};
    rejected([&] { makeGeometryGenerationPlan(max, 1, huge); }); // first + count must not wrap
    ResolvedClusterSelection batch{1, true, {{0, 0, 192}}, 192};
    expect(makeGeometryGenerationPlan(195, 1, batch).workgroupsX == 1, "64 triangles should fill one workgroup.");
    batch.ranges[0].indexCount = 195;
    batch.indexCount = 195;
    expect(makeGeometryGenerationPlan(195, 1, batch).workgroupsX == 2, "65 triangles need a guarded tail workgroup.");
    rejected([&] { makeGeometryGenerationPlan(195, 1, batch, 1, {10000, 10000, 1}); });
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) throw std::runtime_error("Expected selection or limits.");
        const std::string name = argv[1];
        if (name == "selection") checkSelection();
        else if (name == "limits") checkLimits();
        else throw std::runtime_error("Unknown geometry copy test.");
        std::cout << "Geometry copy " << name << " passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
