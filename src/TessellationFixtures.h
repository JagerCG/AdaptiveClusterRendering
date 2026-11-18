#pragma once

#include "SceneData.h"

enum class TessellationFixture { Triangle, Plane, ClusterGrid, ShadowStage, Occlusion, ShadowCasters };

// Authored input geometry for subdivision, visibility and shadow checks.
// The planar fixtures use CCW winding and +Z normals. The stages place patches at different depths.
// UV = ((x+1)/2, (y+1)/2): u grows toward +X, v toward +Y, no seams.
// ClusterGrid is a 2x2 patch grid: IDs 0,1 on the bottom row and 2,3 on top.
SceneData makeTessellationFixture(TessellationFixture fixture);
