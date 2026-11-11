# Adaptive Cluster Rendering

UPenn CIS 5650, Fall 2026, Group 16.

A C++ and Vulkan renderer for experimenting with cluster-based geometry. Milestone 1 imports a static mesh, builds flat clusters on the CPU, saves/loads a validated scene, and uploads fully resident geometry and cluster descriptors to a Vulkan viewer. A fixed four-cluster mock uses the same data and upload interfaces.

## Presentations

Presentation files are stored in the `presentations/` folder.

- [Milestone 1](presentations/Milestone1.pptx) — November 11, 2026. Speaker notes include the presentation script.
- Milestone 2 — Coming soon
- Milestone 3 — Coming soon
- Final Presentation — Coming soon

## Build on Windows

- Visual Studio 2022 with **Desktop development with C++** and a Windows SDK
- CMake 3.24 or newer and Git
- [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) with `glslc`, and a GPU driver that supports Vulkan 1.2

Open a new PowerShell after installing the Vulkan SDK. From the repository root:

```powershell
cmake --preset windows-vs2022
cmake --build --preset debug
.\build\Debug\AdaptiveClusterRendering.exe
```

CMake downloads the pinned dependencies on the first configure, compiles the shaders, and places Suzanne next to the executable. Build files and downloaded dependencies are kept in `build/`.

To run from Visual Studio, open `build/AdaptiveClusterRendering.sln` and select **Debug / x64**. If `ALL_BUILD` is selected for startup, right-click `AdaptiveClusterRendering` in Solution Explorer and choose **Set as Startup Project**. Press **F5** to run.

Use `cmake --build --preset release` to build the Release version in `build/Release/`.

## M1 demo

1. Start the viewer. The bundled Suzanne model should show **2,012 vertices, 3,936 triangles, and 117 clusters** in the Model panel.
2. Toggle **Cluster colors**. The same model should show different colors across cluster boundaries.
3. Drag with the left mouse button to orbit and use the wheel to zoom. Press **R** or click **Reset camera** to return to the initial view.
4. Enable **Cluster bounds**. Use the slider to inspect one cluster, or select **-1** for all bounds. The line vertices are generated from the GPU cluster buffer, with depth testing.
5. Use **Save ACR...** to save the preprocessed scene, then **Open OBJ / ACR...** to load it again. ACR loading preserves the existing partition and IDs without running the cluster builder.
6. Click **Load mock** to switch to four fixed clusters (16 vertices / 16 triangles). Mock and real scenes share validation, serialization and GPU upload.
7. Open another OBJ or ACR file. The viewer fits the camera and updates counts. An invalid file reports an error while keeping the previous scene available.
8. Press **Esc** or close the window to exit.

You can also pass an OBJ path on startup:

```powershell
.\build\Debug\AdaptiveClusterRendering.exe "C:\path\to\model.obj"
```

Use `--cube` for the built-in cube or `--check-environment` to list available Vulkan devices and validation-layer support.

For the fixed mock with colors and bounds already enabled:

```powershell
.\build\Debug\AdaptiveClusterRendering.exe --mock --cluster-colors --bounds
```

Preprocess and validate without creating a Vulkan device or a window:

```powershell
.\build\Debug\AdaptiveClusterRendering.exe --preprocess assets/suzanne.obj build/suzanne.acr
.\build\Debug\AdaptiveClusterRendering.exe --validate build/suzanne.acr
.\build\Debug\AdaptiveClusterRendering.exe --make-mock build/mock.acr
.\build\Debug\AdaptiveClusterRendering.exe build/suzanne.acr --verify-gpu
```

`--verify-gpu` reads the uploaded vertices, indices and cluster records back and compares them byte-for-byte. It is a debugging option, not part of performance measurements. Scene files use the versioned `.acr` format documented in [the M1 interface contract](docs/milestone-1-interface.md); they are not a general-purpose asset format or compatible with other repositories' binary caches.

## Validation

```powershell
cmake --preset windows-vs2022 -DACR_ENABLE_GPU_TESTS=ON
cmake --build --preset debug
ctest --test-dir build -C Debug --output-on-failure
```

CPU tests cover the fixed mock, lossless roundtrip, real OBJ geometry, deterministic clustering, invalid scene data, and malformed/version-mismatched files. Optional GPU tests run hidden windows, verify uploads, replace real/mock scenes, and submit Draw All, cluster color, all-bounds and selected-bounds draws. The GPU tests need a working Vulkan window system. They fail on validation warnings/errors; use **Debug** and install the Khronos validation layer to obtain that coverage. `ACR_ENABLE_GPU_TESTS` is off by default on other machines.

For the CPU tests only: `ctest --test-dir build -C Debug -LE gpu --output-on-failure`. The cached-scene preprocessing test is also CPU-only. No FPS or GPU speedup is asserted by these checks.

## Source layout

| File | Responsibility |
| --- | --- |
| `src/ObjLoader.cpp`, `src/MeshSource.h` | Read OBJ positions, normals and UVs into an indexed mesh. Supply a face normal for corners that lack one. |
| `src/ClusterBuilder.cpp` | Group edge-connected triangles on the CPU. Each cluster has at most 64 triangles, a contiguous index range, and a model-space bounding box. |
| `src/SceneData.*` | Shared scene handoff, fixed mock, strict validation, OBJ/ACR loading, and versioned persistence. |
| `src/ClusterGpu.h`, `src/SceneGpuData.h` | Explicit 48-byte std430 cluster records and the non-owning GPU scene view. |
| `src/VulkanRenderer.cpp`, `shaders/` | Upload geometry and cluster descriptors; depth-tested mesh and SSBO-driven bounds. In cluster color mode, draw each index range with its own color. |
| `src/Viewer.cpp`, `src/OrbitCamera.cpp` | Manage model loading, camera controls, and the Model panel. |

M1 draws all triangles. Clustering and mesh draw submission remain on the CPU; the bounds shader consumes GPU cluster descriptors. M1 does not implement QEM, hierarchy construction/traversal, GPU LOD/culling, Hi-Z, streaming, displacement or shadows. The next milestone can extend `SceneGpuData` and the descriptor layout once its hierarchy contract is agreed.

## Credits

The viewer uses [GLFW](https://github.com/glfw/glfw) for windowing (zlib/libpng), [GLM](https://github.com/g-truc/glm) for math (MIT or Happy Bunny License), [TinyObjLoader](https://github.com/tinyobjloader/tinyobjloader) for OBJ import (MIT), and [Dear ImGui](https://github.com/ocornut/imgui) for the Model panel (MIT). Versions are pinned in `CMakeLists.txt`. The [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) provides the Vulkan development tools.

The bundled OBJ is adapted from the [Khronos Suzanne sample](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Suzanne), credited to UX3D and Norbert Nopper (2017) under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/).
