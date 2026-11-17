#pragma once

#include "SceneData.h"

enum class TessellationFixture { Triangle, Plane, ClusterGrid, ShadowStage, Occlusion };

// Deterministic source geometry for module 07, not tessellated output.
// All fixtures lie in XY at z=0, with +Z unit normals and CCW winding.
// UV = ((x+1)/2, (y+1)/2): u grows toward +X, v toward +Y, no seams.
// ClusterGrid is a 2x2 patch grid: IDs 0,1 on the bottom row and 2,3 on top.
SceneData makeTessellationFixture(TessellationFixture fixture);
