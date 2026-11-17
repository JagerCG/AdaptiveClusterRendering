#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct BenchmarkOptions { uint32_t warmup = 10, samples = 50; };
struct TimingSummary {
    uint32_t count = 0;
    double minimum = 0, median = 0, p95 = 0, maximum = 0, mean = 0;
};
struct BenchmarkSample {
    uint64_t generationRevision = 0;
    std::optional<double> cpuGenerateMs, gpuGenerateMs, gpuRenderMs, gpuMainMs, gpuShadowMs;
};
// Export metadata is grouped by type; every numeric field names its unit.
struct BenchmarkContext {
    std::map<std::string, std::string> labels;
    std::map<std::string, uint64_t> counts;
    std::map<std::string, double> values;
    std::map<std::string, bool> flags;
    std::vector<uint32_t> selectedClusterIds;
    std::vector<float> viewProjection;
};
struct BenchmarkReport {
    BenchmarkOptions options;
    BenchmarkContext context;
    std::vector<BenchmarkSample> samples; // measured samples only; warmup excluded
    uint32_t completedRuns = 0;
    bool complete = false;
};
void validateBenchmarkOptions(const BenchmarkOptions& options);
TimingSummary summarizeTimings(const std::vector<std::optional<double>>& samples);
TimingSummary summarizeBenchmark(const BenchmarkReport& report, std::optional<double> BenchmarkSample::* metric);
std::string benchmarkJson(const BenchmarkReport& report);
void writeBenchmarkReport(const std::filesystem::path& path, const BenchmarkReport& report);
std::string benchmarkUtcStamp();

class BenchmarkSession {
public:
    void begin(BenchmarkContext context, BenchmarkOptions options, bool generated, bool timestamps, bool nonempty);
    void add(const BenchmarkSample& sample);
    void cancel() { active_ = false; }
    bool active() const { return active_; }
    const BenchmarkReport& report() const { return report_; }
private:
    BenchmarkReport report_;
    bool active_ = false, generated_ = false, timestamps_ = false, nonempty_ = false;
    uint64_t lastGeneration_ = 0;
};
