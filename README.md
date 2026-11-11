# Adaptive Cluster Rendering

UPenn CIS 5650, Fall 2026, Group 16.

A C++ and Vulkan renderer for experimenting with cluster-based geometry. The first milestone loads a static mesh, groups its triangles on the CPU, and displays the groups in a Vulkan viewer.

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
4. Use **Open OBJ...** to load another static model. The viewer fits the camera to the new geometry and updates the counts.
5. Press **Esc** or close the window to exit.

You can also pass an OBJ path on startup:

```powershell
.\build\Debug\AdaptiveClusterRendering.exe "C:\path\to\model.obj"
```

Use `--cube` for the built-in cube or `--check-environment` to list available Vulkan devices and validation-layer support.

## Presentations

- [Milestone 1](presentations/Milestone1.pptx), November 11, 2026. Speaker notes include the presentation script.

## Source layout

| File | Responsibility |
| --- | --- |
| `src/ObjLoader.cpp`, `src/MeshSource.h` | Read OBJ positions, normals and UVs into an indexed mesh. Supply a face normal for corners that lack one. |
| `src/ClusterBuilder.cpp` | Group edge-connected triangles on the CPU. Each cluster has at most 64 triangles, a contiguous index range, and a model-space bounding box. |
| `src/VulkanRenderer.cpp`, `shaders/` | Upload the mesh and draw it with depth testing and directional light. In cluster color mode, draw each index range with its own color. |
| `src/Viewer.cpp`, `src/OrbitCamera.cpp` | Manage model loading, camera controls, and the Model panel. |

M1 draws all triangles. The next milestone will build coarser geometry and use the hierarchy for GPU LOD selection.

## Credits

The viewer uses [GLFW](https://github.com/glfw/glfw) for windowing (zlib/libpng), [GLM](https://github.com/g-truc/glm) for math (MIT or Happy Bunny License), [TinyObjLoader](https://github.com/tinyobjloader/tinyobjloader) for OBJ import (MIT), and [Dear ImGui](https://github.com/ocornut/imgui) for the Model panel (MIT). Versions are pinned in `CMakeLists.txt`. The [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) provides the Vulkan development tools.

The bundled OBJ is adapted from the [Khronos Suzanne sample](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Suzanne), credited to UX3D and Norbert Nopper (2017) under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/).
