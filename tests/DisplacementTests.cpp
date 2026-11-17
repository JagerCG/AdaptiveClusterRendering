#include "GeometryValidation.h"
#include "TessellationFixtures.h"
#include <glm/geometric.hpp>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void expect(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
void rejected(const std::function<void()>& action)
{
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    expect(failed, "Invalid height/displacement input or output was accepted.");
}
void sampling()
{
    const HeightField asymmetric{2, 2, {0.f, .25f, .5f, 1.f}, "asymmetric"};
    validateHeightField(asymmetric);
    expect(sampleHeight(asymmetric, 0, 0) == 0 && sampleHeight(asymmetric, 1, 1) == 1 &&
           sampleHeight(asymmetric, 0, 1) == .5 && sampleHeight(asymmetric, .5, .5) == .4375,
           "Bilinear weights / UV orientation are wrong.");
    expect(sampleHeight(asymmetric, -2, 2) == .5 && sampleHeight(asymmetric, 2, -2) == .25, "UV clamping failed.");
    const HeightField single{1, 1, {.5f}, "single"};
    expect(sampleHeight(single, .4, -.7) == .5, "One-pixel sampling failed.");
    const HeightField column{1, 2, {0, 1}, "column"};
    expect(sampleHeight(column, 9, .25) == .25, "One-column sampling failed.");
    expect(displacementBound(asymmetric, {true, .25f}) == .25f &&
           displacementBound(asymmetric, {false, .25f}) == 0 &&
           displacementBound(asymmetric, {true, 0}) == 0 &&
           displacementBound(single, {true, 1}) == 0, "Conservative displacement bound failed.");
    for (auto preset : {HeightPreset::Waves, HeightPreset::Mound, HeightPreset::RampU, HeightPreset::RampV,
                       HeightPreset::Neutral, HeightPreset::Black, HeightPreset::White}) validateHeightField(makeHeightField(preset));
    for (float bad : {-1.f, 1001.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        rejected([&] { validateDisplacement({true, bad}); });
    auto invalid = asymmetric; invalid.width = kMaxHeightDimension + 1;
    rejected([&] { validateHeightField(invalid); });
    invalid = asymmetric; invalid.samples.pop_back();
    rejected([&] { validateHeightField(invalid); });
    for (float bad : {-1.f, 1.01f, std::numeric_limits<float>::quiet_NaN()}) {
        invalid = asymmetric; invalid.samples[1] = bad;
        rejected([&] { validateHeightField(invalid); });
    }
}
void parser(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    const auto path = directory / "heights.pgm";
    const auto write = [&](const std::string& contents) {
        std::ofstream stream(path, std::ios::binary); stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    };
    write("P2\n# comment\n3 1\n2\n0 # black\n1 2\n");
    auto field = loadHeightField(path);
    expect(field.width == 3 && field.height == 1 && field.samples == std::vector<float>({0,.5,1}), "P2 parsing failed.");
    std::string binary = "P5\r\n3 1\r\n255\r\n";
    binary.append("\0\x80\xff", 3);
    write(binary); field = loadHeightField(path);
    expect(field.samples[0] == 0 && field.samples[1] == 128.f/255 && field.samples[2] == 1, "P5 8-bit parsing failed.");
    binary = "P5\n3 1\n65535\n";
    binary.append("\0\0\x80\0\xff\xff", 6);
    write(binary); field = loadHeightField(path);
    expect(field.samples[1] == 32768.f/65535 && field.samples[2] == 1, "P5 16-bit big-endian parsing failed.");
    binary = "P5\n3 1\n255\n";
    binary.append("\x0a\x20\x23", 3); // whitespace and '#' are pixels in a binary raster
    write(binary); field = loadHeightField(path);
    expect(field.samples[0] == 10.f/255 && field.samples[1] == 32.f/255 && field.samples[2] == 35.f/255,
           "Binary raster bytes were misinterpreted as header syntax.");
    write("P5\n1 1\n255\r\n"); // CR delimiter followed by one LF-valued pixel
    expect(loadHeightField(path).samples[0] == 10.f/255, "CR delimiter consumed the first binary pixel.");
    write("P5\n1 1\n255\r\n\n"); // CRLF delimiter followed by the same pixel
    expect(loadHeightField(path).samples[0] == 10.f/255, "CRLF delimiter changed the first binary pixel.");
    for (const std::string bad : {"P3\n1 1\n255\n0", "P2\n0 1\n255\n", "P2\n2049 1\n255\n",
         "P2\n1 1\n0\n0", "P2\n1 1\n65536\n0", "P2\n1 1\n1\n2", "P2\n2 1\n1\n0",
         "P2\n1 1\n1\n0 1", "P2\n1 1\n1\n-1", "P5\n1 1\n65535\nX", "P5\n1 1\n255\nXX"}) {
        write(bad); rejected([&] { loadHeightField(path); });
    }
}
void verifier()
{
    auto source = makeTessellationFixture(TessellationFixture::Triangle);
    const auto selection = resolveClusterSelection(source.dataset.clusters, 3, 1, {1, ClusterSelectionMode::All, {}});
    const auto ramp = makeHeightField(HeightPreset::RampU);
    const auto neutral = makeHeightField(HeightPreset::Neutral);
    const auto white = makeHeightField(HeightPreset::White);
    const std::vector<uint32_t> indices{0,1,2};
    auto good = source.mesh.vertices;
    // Hand-authored analytic surface: z = 0.25*x, not a copy of the shader implementation.
    good[0].position.z = -.25f; good[1].position.z = .25f; good[2].position.z = -.25f;
    for (auto& v : good) v.normal = glm::normalize(glm::vec3(-.25,0,1));
    validateGeneratedGeometry(source, selection, 1, good, indices, &ramp, {true,.25f});
    for (auto& v : source.mesh.vertices) v.normal *= 7; // displacement uses a normalized direction
    validateGeneratedGeometry(source, selection, 1, good, indices, &ramp, {true,.25f});
    validateGeneratedGeometry(source, selection, 1, source.mesh.vertices, indices, &neutral, {true,.25f});
    validateGeneratedGeometry(source, selection, 1, source.mesh.vertices, indices, &ramp, {true,0});
    validateGeneratedGeometry(source, selection, 1, source.mesh.vertices, indices, &ramp, {false,.25f});
    auto bad = good; bad[0].position.z += .01f;
    rejected([&] { validateGeneratedGeometry(source, selection, 1, bad, indices, &ramp, {true,.25f}); });
    bad = good; bad[0].normal = {0,0,1};
    rejected([&] { validateGeneratedGeometry(source, selection, 1, bad, indices, &ramp, {true,.25f}); });
    bad = good; bad[0].texcoord.x = .1f;
    rejected([&] { validateGeneratedGeometry(source, selection, 1, bad, indices, &ramp, {true,.25f}); });
    bad = good; bad[0].normal.x = std::numeric_limits<float>::quiet_NaN();
    rejected([&] { validateGeneratedGeometry(source, selection, 1, bad, indices, &ramp, {true,.25f}); });
    for (auto& v : source.mesh.vertices) v.normal = {0,0,0};
    good = source.mesh.vertices;
    for (auto& v : good) v.normal = {0,0,1};
    validateGeneratedGeometry(source, selection, 1, good, indices, &white, {true,.25f});
}
} // namespace
int main(int argc, char** argv)
{
    try {
        if (argc < 2) throw std::runtime_error("Expected sampling, parser or verifier.");
        const std::string name = argv[1];
        if (name == "sampling") sampling();
        else if (name == "parser" && argc == 3) parser(argv[2]);
        else if (name == "verifier") verifier();
        else throw std::runtime_error("Unknown displacement test.");
        std::cout << "Displacement " << name << " passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
