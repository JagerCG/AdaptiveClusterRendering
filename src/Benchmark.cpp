#include "Benchmark.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <locale>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
void checkTime(const std::optional<double>& value)
{
    require(!value || (std::isfinite(*value) && *value >= 0), "Benchmark timing must be finite and nonnegative.");
}
std::string quote(const std::string& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c; // retain UTF-8 bytes
    }
    out << '"';
    return out.str();
}
void optionalTime(std::ostream& out, const std::optional<double>& value)
{
    checkTime(value);
    if (value) out << *value; else out << "null";
}
} // namespace

void validateBenchmarkOptions(const BenchmarkOptions& options)
{
    require(options.warmup >= 1 && options.warmup <= 1000, "Benchmark warmup must be 1..1000 runs.");
    require(options.samples >= 2 && options.samples <= 10000, "Benchmark samples must be 2..10000 runs.");
}
TimingSummary summarizeTimings(const std::vector<std::optional<double>>& samples)
{
    std::vector<double> values;
    for (const auto& sample : samples) { checkTime(sample); if (sample) values.push_back(*sample); }
    TimingSummary result;
    if (values.empty()) return result;
    std::sort(values.begin(), values.end());
    result.count = static_cast<uint32_t>(values.size());
    result.minimum = values.front(); result.maximum = values.back();
    result.median = values[values.size()/2];
    if (values.size() % 2 == 0) result.median = (values[values.size()/2 - 1] + result.median) / 2;
    result.p95 = values[static_cast<size_t>(std::ceil(.95 * values.size())) - 1]; // nearest rank
    result.mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    return result;
}
TimingSummary summarizeBenchmark(const BenchmarkReport& report, std::optional<double> BenchmarkSample::* metric)
{
    std::vector<std::optional<double>> values;
    for (const auto& sample : report.samples) values.push_back(sample.*metric);
    return summarizeTimings(values);
}
void BenchmarkSession::begin(BenchmarkContext context, BenchmarkOptions options, bool generated, bool timestamps, bool nonempty)
{
    validateBenchmarkOptions(options);
    require(!active_, "A benchmark is already running.");
    for (const auto& entry : context.values) require(std::isfinite(entry.second), "Nonfinite benchmark metadata.");
    for (float value : context.viewProjection) require(std::isfinite(value), "Nonfinite benchmark camera.");
    BenchmarkReport next;
    next.options = options; next.context = std::move(context);
    next.samples.reserve(options.samples);
    report_ = std::move(next);
    generated_ = generated; timestamps_ = timestamps; nonempty_ = nonempty;
    lastGeneration_ = 0; active_ = true;
}
void BenchmarkSession::add(const BenchmarkSample& sample)
{
    require(active_, "No active benchmark accepts this sample.");
    checkTime(sample.gpuMainMs); checkTime(sample.gpuShadowMs);
    checkTime(sample.cpuGenerateMs); checkTime(sample.gpuGenerateMs); checkTime(sample.gpuRenderMs);
    require(sample.cpuGenerateMs.has_value() == generated_, "Benchmark CPU generation timing has the wrong scope.");
    require(sample.gpuGenerateMs.has_value() == (generated_ && timestamps_ && nonempty_), "Benchmark GPU generation availability changed.");
    require(sample.gpuRenderMs.has_value() == timestamps_, "Benchmark render timestamp availability changed.");
    require(generated_ ? sample.generationRevision > lastGeneration_ : sample.generationRevision == 0,
            "Benchmark reused cached geometry instead of regenerating it.");
    if (report_.completedRuns >= report_.options.warmup) report_.samples.push_back(sample);
    lastGeneration_ = sample.generationRevision;
    ++report_.completedRuns;
    report_.complete = report_.completedRuns == report_.options.warmup + report_.options.samples;
    if (report_.complete) active_ = false;
}
std::string benchmarkJson(const BenchmarkReport& report)
{
    validateBenchmarkOptions(report.options);
    require(report.complete && report.samples.size() == report.options.samples &&
            report.completedRuns == report.options.warmup + report.options.samples, "Cannot export an incomplete benchmark.");
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(12) << "{\n  \"schema_version\": 1,\n  \"warmup_runs\": " << report.options.warmup
        << ",\n  \"measured_runs\": " << report.samples.size() << ",\n  \"completed_runs\": " << report.completedRuns;
    const auto map = [&](const char* name, const auto& entries, const auto& emit) {
        out << ",\n  " << quote(name) << ": {";
        bool first = true;
        for (const auto& entry : entries) {
            if (!first) out << ',';
            out << "\n    " << quote(entry.first) << ": "; emit(entry.second); first = false;
        }
        out << "\n  }";
    };
    map("labels", report.context.labels, [&](const auto& v) { out << quote(v); });
    map("counts", report.context.counts, [&](uint64_t v) { out << v; });
    map("values", report.context.values, [&](double v) { require(std::isfinite(v), "Nonfinite report metadata."); out << v; });
    map("flags", report.context.flags, [&](bool v) { out << (v ? "true" : "false"); });
    out << ",\n  \"selected_cluster_ids\": [";
    for (size_t i = 0; i < report.context.selectedClusterIds.size(); ++i) { if (i) out << ','; out << report.context.selectedClusterIds[i]; }
    out << "],\n  \"view_projection_column_major\": [";
    for (size_t i = 0; i < report.context.viewProjection.size(); ++i) {
        if (i) out << ',';
        require(std::isfinite(report.context.viewProjection[i]), "Nonfinite report camera.");
        out << report.context.viewProjection[i];
    }
    out << "],\n  \"summary_ms\": {";
    const auto summary = [&](const char* name, std::optional<double> BenchmarkSample::* metric, bool comma) {
        if (comma) out << ',';
        const auto stats = summarizeBenchmark(report, metric);
        out << "\n    " << quote(name) << ": ";
        if (!stats.count) { out << "null"; return; }
        out << "{\"count\":" << stats.count << ",\"min\":" << stats.minimum << ",\"median\":" << stats.median
            << ",\"p95\":" << stats.p95 << ",\"max\":" << stats.maximum << ",\"mean\":" << stats.mean << '}';
    };
    summary("cpu_generate_and_wait", &BenchmarkSample::cpuGenerateMs, false);
    summary("gpu_generate", &BenchmarkSample::gpuGenerateMs, true);
    summary("gpu_render", &BenchmarkSample::gpuRenderMs, true);
    summary("gpu_main", &BenchmarkSample::gpuMainMs, true);
    summary("gpu_shadow", &BenchmarkSample::gpuShadowMs, true);
    out << "\n  },\n  \"samples\": [";
    for (size_t i = 0; i < report.samples.size(); ++i) {
        const auto& sample = report.samples[i];
        if (i) out << ',';
        out << "\n    {\"generation_revision\":" << sample.generationRevision << ",\"cpu_generate_ms\":";
        optionalTime(out, sample.cpuGenerateMs);
        out << ",\"gpu_generate_ms\":"; optionalTime(out, sample.gpuGenerateMs);
        out << ",\"gpu_render_ms\":"; optionalTime(out, sample.gpuRenderMs);
        out << ",\"gpu_main_ms\":"; optionalTime(out, sample.gpuMainMs);
        out << ",\"gpu_shadow_ms\":"; optionalTime(out, sample.gpuShadowMs); out << '}';
    }
    out << "\n  ]\n}\n";
    return out.str();
}
void writeBenchmarkReport(const std::filesystem::path& path, const BenchmarkReport& report)
{
    require(path.extension() == ".json", "Benchmark output must use a .json extension.");
    const auto contents = benchmarkJson(report); // Validate before touching the output file.
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open benchmark output: " + path.u8string());
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    if (!out) throw std::runtime_error("Cannot write benchmark output: " + path.u8string());
}
std::string benchmarkUtcStamp()
{
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
    return out.str();
}
