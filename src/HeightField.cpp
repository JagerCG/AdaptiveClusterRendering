#include "HeightField.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace {
bool space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
void skip(std::istream& stream)
{
    while (stream) {
        if (space(stream.peek())) stream.get();
        else if (stream.peek() == '#') stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        else break;
    }
}
std::string token(std::istream& stream)
{
    skip(stream);
    std::string result;
    while (stream && stream.peek() != EOF && !space(stream.peek()) && stream.peek() != '#') {
        if (result.size() == 16) throw std::runtime_error("PGM token is too long.");
        result.push_back(static_cast<char>(stream.get()));
    }
    if (result.empty()) throw std::runtime_error("Truncated PGM header or samples.");
    return result;
}
uint32_t number(std::istream& stream)
{
    const auto text = token(stream);
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') throw std::runtime_error("PGM requires unsigned integer values.");
        value = value * 10 + uint32_t(c - '0');
        if (value > std::numeric_limits<uint32_t>::max()) throw std::runtime_error("PGM integer overflow.");
    }
    return static_cast<uint32_t>(value);
}
void dimensions(uint32_t w, uint32_t h)
{
    if (!w || !h || w > kMaxHeightDimension || h > kMaxHeightDimension)
        throw std::runtime_error("Height map dimensions must be 1..2048 (maximum 16 MiB of floats).");
}
} // namespace

void validateHeightField(const HeightField& field)
{
    dimensions(field.width, field.height);
    if (field.samples.size() != size_t(field.width) * field.height)
        throw std::runtime_error("Height map sample count does not match its dimensions.");
    for (float h : field.samples) if (!std::isfinite(h) || h < 0 || h > 1)
        throw std::runtime_error("Height samples must be finite linear values in [0,1].");
}
void validateDisplacement(const DisplacementSettings& settings)
{
    if (!std::isfinite(settings.amplitude) || settings.amplitude < 0 || settings.amplitude > 1000)
        throw std::runtime_error("Displacement amplitude must be finite and in [0,1000] model units.");
}
bool sameDisplacement(const DisplacementSettings& a, const DisplacementSettings& b)
{
    return a.enabled == b.enabled && a.amplitude == b.amplitude;
}
HeightField makeHeightField(HeightPreset preset)
{
    HeightField result;
    result.width = result.height = 33;
    switch (preset) {
    case HeightPreset::Waves: result.name = "Waves"; break;
    case HeightPreset::Mound: result.name = "Mound"; break;
    case HeightPreset::RampU: result.name = "Ramp U"; break;
    case HeightPreset::RampV: result.name = "Ramp V"; break;
    case HeightPreset::Neutral: result.name = "Neutral (0.5)"; break;
    case HeightPreset::Black: result.name = "Black (0)"; break;
    case HeightPreset::White: result.name = "White (1)"; break;
    default: throw std::runtime_error("Unknown height map preset.");
    }
    for (uint32_t y = 0; y < result.height; ++y) for (uint32_t x = 0; x < result.width; ++x) {
        const double u = double(x) / (result.width - 1), v = double(y) / (result.height - 1);
        double h = 0.5;
        switch (preset) {
        case HeightPreset::Waves: h += 0.5 * std::sin(6.283185307179586 * u) * std::sin(6.283185307179586 * v); break;
        case HeightPreset::Mound: { const double r = std::max(0.0, 1 - 4 * ((u - .5)*(u - .5) + (v - .5)*(v - .5))); h += .5*r*r; break; }
        case HeightPreset::RampU: h = u; break;
        case HeightPreset::RampV: h = v; break;
        case HeightPreset::Black: h = 0; break;
        case HeightPreset::White: h = 1; break;
        default: break;
        }
        result.samples.push_back(static_cast<float>(std::clamp(h, 0.0, 1.0)));
    }
    return result;
}
HeightField loadHeightField(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open height map: " + path.u8string());
    const auto magic = token(stream);
    if (magic != "P2" && magic != "P5") throw std::runtime_error("Height maps must be PGM P2 or P5 files.");
    HeightField field;
    field.width = number(stream); field.height = number(stream);
    dimensions(field.width, field.height);
    const uint32_t maximum = number(stream);
    if (!maximum || maximum > 65535) throw std::runtime_error("PGM maximum value must be 1..65535.");
    if (magic == "P5") {
        const int separator = stream.get();
        if (!space(separator)) throw std::runtime_error("Missing PGM binary raster separator.");
        if (separator == '\r' && stream.peek() == '\n') {
            // A CR-only delimiter may be followed by a pixel whose byte is LF.
            // Accept CRLF without eating that valid pixel: the raster size decides.
            const auto raster = stream.tellg();
            stream.seekg(0, std::ios::end);
            const auto remaining = stream.tellg() - raster;
            stream.seekg(raster);
            const auto expected = std::streamoff(field.width) * field.height * (maximum > 255 ? 2 : 1);
            if (remaining == expected + 1) stream.get();
        }
    }
    field.samples.reserve(size_t(field.width) * field.height);
    for (size_t i = 0; i < size_t(field.width) * field.height; ++i) {
        uint32_t sample;
        if (magic == "P2") sample = number(stream);
        else {
            const int high = stream.get(), low = maximum > 255 ? stream.get() : 0;
            if (high == EOF || low == EOF) throw std::runtime_error("Truncated PGM binary raster.");
            sample = maximum > 255 ? uint32_t(high * 256 + low) : uint32_t(high);
        }
        if (sample > maximum) throw std::runtime_error("PGM sample exceeds its declared maximum.");
        field.samples.push_back(float(sample) / float(maximum));
    }
    if (magic == "P2") skip(stream);
    if (stream.peek() != EOF) throw std::runtime_error("Unexpected trailing data in PGM (one image only).");
    field.name = path.filename().u8string();
    return field;
}
double sampleHeight(const HeightField& field, double u, double v)
{
    // Endpoint convention: u=0/1 samples the first/last texel center.
    const double x = std::clamp(u, 0.0, 1.0) * (field.width - 1), y = std::clamp(v, 0.0, 1.0) * (field.height - 1);
    const auto x0 = uint32_t(x), y0 = uint32_t(y);
    const auto x1 = std::min(x0 + 1, field.width - 1), y1 = std::min(y0 + 1, field.height - 1);
    const auto at = [&](uint32_t xx, uint32_t yy) { return double(field.samples[size_t(yy) * field.width + xx]); };
    const double tx = x - x0, ty = y - y0;
    return ((1-tx)*at(x0,y0) + tx*at(x1,y0))*(1-ty) + ((1-tx)*at(x0,y1) + tx*at(x1,y1))*ty;
}
float displacementBound(const HeightField& field, const DisplacementSettings& settings)
{
    validateDisplacement(settings);
    if (!settings.enabled || settings.amplitude == 0) return 0;
    validateHeightField(field);
    const auto range = std::minmax_element(field.samples.begin(), field.samples.end());
    return settings.amplitude * std::max(std::abs(2 * *range.first - 1), std::abs(2 * *range.second - 1));
}
