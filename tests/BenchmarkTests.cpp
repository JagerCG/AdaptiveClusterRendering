#include "Benchmark.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
template<class F> void rejects(F action)
{
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Invalid benchmark operation was accepted.");
}
void stats()
{
    const auto s = summarizeTimings({6., 1., std::nullopt, 8., 2.});
    require(s.count == 4 && s.minimum == 1 && s.median == 4 && s.p95 == 8 && s.maximum == 8 && s.mean == 4.25,
            "Even median or optional-value handling is incorrect.");
    require(summarizeTimings({9., 1., 5.}).median == 5, "Odd median is incorrect.");
    std::vector<std::optional<double>> ordered;
    for (unsigned i = 1; i <= 100; ++i) ordered.push_back(double(i));
    require(summarizeTimings(ordered).p95 == 95, "p95 must use nearest rank.");
    require(summarizeTimings({std::nullopt}).count == 0, "Missing timestamps must not become zero.");
    rejects([] { summarizeTimings({-1.}); });
    rejects([] { summarizeTimings({std::numeric_limits<double>::infinity()}); });
    rejects([] { summarizeTimings({std::numeric_limits<double>::quiet_NaN()}); });
}
void sessions()
{
    BenchmarkSession session;
    rejects([&] { session.begin({}, {0, 2}, true, true, true); });
    rejects([&] { session.begin({}, {1, 1}, true, true, true); });
    session.begin({}, {2, 2}, true, true, true);
    rejects([&] { session.begin({}, {2, 2}, true, true, true); });
    session.add({10, 100., 100., 100.});
    rejects([&] { session.add({10, 100., 100., 100.}); }); // cached generation
    rejects([&] { session.add({11, 100., std::nullopt, 100.}); }); // lost query
    session.add({11, 100., 100., 100.});
    require(session.report().samples.empty(), "Warmup polluted the measured samples.");
    session.add({12, 2., 3., 4.});
    session.add({13, 6., 7., 8.});
    require(!session.active() && session.report().complete && session.report().completedRuns == 4,
            "Session did not finish at the requested count.");
    require(summarizeBenchmark(session.report(), &BenchmarkSample::cpuGenerateMs).median == 4, "Warmup was included in summary.");
    rejects([&] { session.add({14, 0., 0., 0.}); });
    session.begin({}, {1, 2}, true, true, false); // empty selection: no dispatch, still a render pass
    for (unsigned i = 1; i <= 3; ++i) session.add({i, 1., std::nullopt, 1.});
    require(summarizeBenchmark(session.report(), &BenchmarkSample::gpuGenerateMs).count == 0, "Empty dispatch fabricated a timing.");
    session.begin({}, {1, 2}, false, false, true); // unsupported timestamp / source path
    for (unsigned i = 0; i < 3; ++i) session.add({});
    require(session.report().complete, "Source-only unsupported timestamp measurement failed.");
    session.begin({}, {1, 2}, true, false, true);
    session.add({1, 1., std::nullopt, std::nullopt});
    session.cancel();
    require(!session.active() && !session.report().complete, "Cancel published a partial report.");
    rejects([&] { benchmarkJson(session.report()); });
}
void exportReport(const std::filesystem::path& directory)
{
    BenchmarkContext context;
    context.labels["quoted"] = "a\"b\\c\n\t";
    context.labels["unicode"] = u8"测量";
    BenchmarkSession session;
    session.begin(context, {1, 2}, false, true, true);
    session.add({0, {}, {}, 99.});
    session.add({0, {}, {}, 1.});
    session.add({0, {}, {}, 3.});
    const auto path = directory / "report.json";
    writeBenchmarkReport(path, session.report());
    std::ifstream input(path, std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(input)), {});
    require(json.find("a\\\"b\\\\c\\u000a\\u0009") != std::string::npos, "JSON control strings were not escaped.");
    require(json.find(u8"测量") != std::string::npos, "UTF-8 metadata was lost.");
    require(json.find("\"gpu_generate\": null") != std::string::npos, "N/A summary was not null.");
    require(json.find("99") == std::string::npos, "Warmup leaked into exported samples.");
    rejects([&] { writeBenchmarkReport(directory / "bad.txt", session.report()); });
    session.begin({}, {1, 2}, false, true, true);
    rejects([&] { writeBenchmarkReport(path, session.report()); });
    std::ifstream unchanged(path, std::ios::binary);
    require(std::string((std::istreambuf_iterator<char>(unchanged)), {}) == json, "Invalid export overwrote an existing report.");
}
}
int main(int argc, char** argv)
{
    try {
        if (argc < 2) throw std::runtime_error("Choose stats, session, or export.");
        const std::string mode = argv[1];
        if (mode == "stats") stats();
        else if (mode == "session") sessions();
        else if (mode == "export" && argc == 3) exportReport(argv[2]);
        else throw std::runtime_error("Unknown test.");
        std::cout << "Benchmark " << mode << " passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
