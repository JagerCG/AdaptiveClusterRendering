#pragma once

#include <string>

struct SceneData;

struct ViewerOptions {
    bool clusterColors = false;
    bool showBounds = false;
    bool verifyGpu = false;
    bool smokeTest = false; // hidden, bounded run with scene replacement and all draw modes
};

int runViewer(SceneData scene, std::string modelName, ViewerOptions options = {});
