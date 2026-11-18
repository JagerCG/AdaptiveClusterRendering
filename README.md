# Adaptive Cluster Rendering

UPenn CIS 5650, Fall 2026, Group 16.

A C++ and Vulkan renderer for experimenting with cluster-based geometry. The first milestone loads a static mesh, groups its triangles on the CPU, and displays the groups in a Vulkan viewer.

## Presentations

Presentation files are stored in the `presentations/` folder.

- [Milestone 1](presentations/Milestone1.pptx) — November 11, 2026
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
2. Use **Colors** to switch between **Shaded**, **Cluster**, and **Group**. At level 0, a group contains up to eight edge-connected clusters. Higher levels group complete replacement regions. Grouping alone does not change the geometry.
3. Drag with the left mouse button to orbit and use the wheel to zoom. Press **R** or click **Reset camera** to return to the initial view.
4. Use **Open OBJ...** to load another static model. The viewer fits the camera to the new geometry and updates the counts.
5. Press **Esc** or close the window to exit.

You can also pass an OBJ path on startup:

```powershell
.\build\Debug\AdaptiveClusterRendering.exe "C:\path\to\model.obj"
```

Use `--cube` for the built-in cube or `--check-environment` to list available Vulkan devices and validation-layer support.

## LOD building

Set **Keep (%)** and click **Build LOD**. The ratio applies to each group's simplification at each build step. Use **LOD level** to inspect the generated geometry: level 0 is the original mesh, and higher levels are coarser. The camera stays fixed while switching. **Wireframe**, cluster colors and group colors work at every level.

The CPU builder groups neighboring regions, simplifies each group with endpoint [QEM](https://www.mgarland.org/research/quadrics.html) collapses, and re-clusters the result. Each coarse node records its geometry range and the finer nodes it replaces. Higher levels combine whole nodes into replacement trees; disconnected regions can retain separate roots. Building stops at eight levels or when a step no longer reduces triangle count. Regions that cannot simplify retain their geometry.

Connectivity follows original position IDs, while render vertices retain separate UV and normal records. Shared group positions and their boundary normals are preserved. Open boundaries, local flips and connectivity checks can prevent the requested ratio from being reached. **Level error** accumulates vertex-displacement estimates; it is not a pixel error or a measured surface-distance bound. Rebuilding always starts from level 0.

## GPU LOD selection

After **Build LOD**, use **LOD mode** to compare:

1. **Manual level**: draw one complete level, chosen with **LOD level**.
2. **Distance LOD**: choose one complete level automatically as the camera moves away. Each doubling of distance beyond three model radii advances one level.
3. **GPU local LOD**: choose coarse or fine replacement regions independently using projected error. Increase **Error px** to permit coarser geometry; decrease it to retain detail. Use **Colors / LOD** and **Wireframe** to see which levels draw together.

The panel reports selected clusters and triangles from the last completed GPU frame, followed by cluster counts for each active level. All levels are uploaded together when loading or rebuilding; changing the mode, level or threshold does not upload geometry again.

A compute shader evaluates each cluster's node and its ancestors. It selects a node when its projected error is acceptable and no ancestor can replace it; finest nodes are the fallback. The projection uses the stored error, camera direction, viewport height and the nearest depth of the node's bounds. This is an approximate selection rule, not a guarantee that every image pixel differs by less than the threshold.

The GPU writes indexed indirect draw commands, which rendering consumes directly. CPU readback supplies only the panel statistics. This first implementation reserves one command per cluster across all levels and sets rejected commands to zero indices. It does not yet compact commands or skip entire subtrees during selection, and no performance improvement is claimed without measurement. Visibility culling is not connected yet.

## Presentations

- [Milestone 1](presentations/Milestone1.pptx), November 11, 2026. Speaker notes include the presentation script.

## Source layout

| File | Responsibility |
| --- | --- |
| `src/ObjLoader.cpp`, `src/MeshSource.h` | Read OBJ positions, normals and UVs into an indexed mesh. Supply a face normal for corners that lack one. |
| `src/ClusterBuilder.cpp` | Group edge-connected triangles on the CPU. Each cluster has at most 64 triangles, a contiguous index range, and a model-space bounding box. |
| `src/ClusterGrouping.cpp` | Group adjacent clusters on the CPU and record their membership and combined bounds. |
| `src/MeshSimplifier.cpp` | Simplify an indexed mesh with QEM edge collapses, compact the result and rebuild normals. |
| `src/LodBuilder.cpp`, `src/LodBuilder.h` | Build coarse geometry per group and record levels, replacement links, bounds and accumulated errors. |
| `src/GpuLodSelection.cpp`, `shaders/lod_select.comp` | Pack all levels, select geometry on the GPU, and produce indirect commands and counts. |
| `src/VulkanRenderer.cpp`, `shaders/mesh.vert`, `shaders/mesh.frag` | Upload geometry and render the GPU-produced commands with depth testing, directional light and debug colors. |
| `src/Viewer.cpp`, `src/OrbitCamera.cpp` | Manage model loading, camera controls, and the Model panel. |

GPU LOD requires a graphics queue with compute support and the Vulkan `drawIndirectFirstInstance` feature. Multi-draw indirect is used when supported; otherwise commands are submitted individually.

## Credits

The viewer uses [GLFW](https://github.com/glfw/glfw) for windowing (zlib/libpng), [GLM](https://github.com/g-truc/glm) for math (MIT or Happy Bunny License), [TinyObjLoader](https://github.com/tinyobjloader/tinyobjloader) for OBJ import (MIT), and [Dear ImGui](https://github.com/ocornut/imgui) for the Model panel (MIT). Versions are pinned in `CMakeLists.txt`. The [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) provides the Vulkan development tools.

The bundled OBJ is adapted from the [Khronos Suzanne sample](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Suzanne), credited to UX3D and Norbert Nopper (2017) under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/).
