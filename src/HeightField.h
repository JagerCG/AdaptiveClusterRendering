#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Linear (not sRGB) heights. Row zero corresponds to v=0; UVs clamp to [0,1].
struct HeightField {
    uint32_t width = 0, height = 0;
    std::vector<float> samples;
    std::string name;
};
enum class HeightPreset { Waves, Mound, RampU, RampV, Neutral, Black, White };
struct DisplacementSettings { bool enabled = false; float amplitude = 0.25f; };
constexpr uint32_t kMaxHeightDimension = 2048;

void validateHeightField(const HeightField& field);
void validateDisplacement(const DisplacementSettings& settings);
bool sameDisplacement(const DisplacementSettings& a, const DisplacementSettings& b);
HeightField makeHeightField(HeightPreset preset);
HeightField loadHeightField(const std::filesystem::path& path); // PGM P2/P5, 8/16 bit
// These CPU functions support validation/metadata only; the shader samples the image.
// sampleHeight requires a validated field and finite UVs (as provided by SceneData).
double sampleHeight(const HeightField& field, double u, double v);
float displacementBound(const HeightField& field, const DisplacementSettings& settings);
