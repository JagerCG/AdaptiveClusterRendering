#pragma once

#include <filesystem>
#include <cstdint>
#include "HeightField.h"
#include "Benchmark.h"
#include "RenderPlanning.h"
#include "ViewConfig.h"

struct LoadedScene;

struct ViewerOptions {
    RenderSettings renderSettings;
    bool renderSmoke = false;
    bool shadowCullSmoke = false;
    bool hidePanel = false;
    std::filesystem::path saveConfigPath;
    std::optional<ViewConfig> initialView;
    uint32_t geometryLevel = 0;
    bool clusterColors = false;
    bool showBounds = false;
    bool verifyGpu = false;
    bool computeGeometry = false;
    uint32_t edgeSegments = 1;
    bool wireframe = false;
    DisplacementSettings displacement;
    std::filesystem::path heightMapPath;
    int selectionPreset = 0; // all / first+last / empty / last+first
    BenchmarkOptions benchmarkOptions;
    std::filesystem::path benchmarkPath; // nonempty: hidden fixed-input measurement, then exit
    std::filesystem::path reportDirectory; // interactive exports
    std::filesystem::path assetDirectory = "assets"; // bundled models, height maps and views
    bool smokeTest = false; // hidden, bounded run with scene replacement and all draw modes
    std::filesystem::path smokeRoundtripPath; // optional on-disk save/mock/reload verification
    std::filesystem::path hierarchyRoundtripPath;
};

int runViewer(LoadedScene loaded, ViewerOptions options = {});
