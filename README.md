# Adaptive Cluster Rendering

UPenn CIS 5650, Fall 2026, Group 16.

A C++ and Vulkan renderer for experimenting with cluster-based geometry. Milestone 1 imports a static mesh, builds flat clusters on the CPU, saves/loads a validated scene, and uploads fully resident geometry and cluster descriptors to a Vulkan viewer. A fixed four-cluster mock uses the same data and upload interfaces.

## Development handoff

The `main` branch runs M2 Part 2 (02 storage and 07 subdivision/displacement) and Part 3 (05 rasterization, 06 depth/material display and timings, 08 shadows). OBJ imports use the M1 flat clusters. The complete pre-split M2 implementation is preserved on [Qingyao_Tian](https://github.com/JagerCG/AdaptiveClusterRendering/tree/Qingyao_Tian).

From the repository root, launch `build/Release/AdaptiveClusterRendering.exe --view-config assets/views/m2-shadow-casters.acrview` for the off-camera shadow demo. [Demo and acceptance checks](docs/milestone-2-acceptance.md)

The [M2 Part 2 foundation and integration contract](docs/milestone-2-part2-contract.md)
defines the working interfaces for storage and compute-based tessellation/displacement,
including resource ownership and the handoff to rendering. It records the synchronized
baseline and verification results. M2 Part 2 now includes Step 2 storage diagnostics
and verified save/reload, Step 3 deterministic fixtures and cluster-selection
previews, Step 4 compute-to-render integration, Step 5 uniform GPU subdivision
with wireframe comparison, Step 6 GPU height-map displacement, and Step 7 fixed-input
measurement, report export and a complete standalone demo.

Start with the [M2 Part 2 demo guide](docs/milestone-2-part2-demo.md) for the short
walkthrough and the original standalone measurement commands. Recorded Part 2 Release
results are in the [performance snapshot](docs/milestone-2-part2-performance.md).

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
The upstream Ninja configure presets remain available with build presets `ninja-debug`
and `ninja-release`; configure each matching preset in an MSVC developer environment first.

## Viewer controls

The sidebar shows the active model, source triangles and triangles drawn by the camera. Wireframe and cluster colors stay available above four tabs.

| Tab | Controls |
| --- | --- |
| Scene | Open models and views, save and verify ACR files, switch stored levels, inspect clusters and storage. |
| Surface | Enable GPU subdivision, choose 1/2/4/8 segments per edge and adjust height displacement. |
| Shadows | Switch shaded/depth views, change the directional light and inspect shadow casters. |
| Stats | Run or cancel benchmarks, inspect generated buffers and see upload timings. |

Choose **Scene > Demo scene > Blue mound** for the presentation grid. It opens on **Surface**
with four segments per edge, the Mound height map and amplitude 0.80. Toggle **Height displacement**
to compare the flat grid and the mound at the same 128 drawn triangles. The build copies
the presets to `assets/views` beside the executable. To launch the same view from the repository root:

```powershell
.\build\Release\AdaptiveClusterRendering.exe --view-config assets/views/m2-grid-mound.acrview
```

## M1 demo

1. Start the viewer. The bundled Suzanne model should show **2,012 vertices, 3,936 triangles, and 117 clusters** across the sidebar summary, **Scene > Cluster inspection** and **Stats > GPU output**.
2. Toggle **Cluster colors**. The same model should show different colors across cluster boundaries.
3. Drag with the left mouse button to orbit and use the wheel to zoom. Press **R** or click **Reset camera** to return to the initial view.
4. Enable **Cluster bounds**. Use the slider to inspect one cluster, or select **-1** for all bounds. The line vertices are generated from the GPU cluster buffer, with depth testing.
5. Use **Save + verify ACR...** to save the preprocessed scene and compare a reload against every persisted field, then **Reload saved ACR** for a fixed-view comparison. ACR loading preserves the existing partition and IDs without running the cluster builder.
6. Choose **Scene > Demo scene > Load mock** to switch to four fixed clusters (16 vertices / 16 triangles). Mock and real scenes share validation, serialization and GPU upload.
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

`--verify-gpu` compares the resident vertex, index, cluster and hierarchy buffers against the CPU records byte for byte. The [M1 interface contract](docs/milestone-1-interface.md) describes the flat ACR v1 layout.

## Stored geometry and camera depth

Flat input saves as ACR v1. Existing ACR v2 files retain their stored levels, coarse indices, cluster bounds and child links across loading, device upload and saving. **Stored ACR geometry levels** selects one resident level from a multilevel cache. Opening an OBJ builds the original flat cluster partition.

The main pass draws the fixed cluster selection. `VulkanRenderer::cameraDepth()` exposes its sampled D32 image, extent, view-projection matrix, scene revision and frame serial to a future visibility pass. Empty pixels have depth 1. The camera-depth display shows near surfaces as bright. Directional shadow rendering can filter the same input independently in light space. The shadow pass also accepts off-camera geometry.

## M2 Part 2: storage diagnostics demo

1. Start with Suzanne. **Scene > Storage** identifies the OBJ source and its
   file size. Hover the source name for its full path and a byte count for its exact
   value. Expand **Payload breakdown** for vertex, rendering-index and cluster bytes.
2. Enable cluster colors/bounds, select one cluster, and adjust the camera.
   **Save + verify ACR...** writes an `.acr` file, reads it back, and checks exact
   geometry, UVs/normals, original/reordered indices, position IDs, cluster ranges,
   bounds and capacity. A success message appears only after that comparison passes.
3. Click **Reload saved ACR**. This uploads the saved data while preserving the
   current camera, level and cluster selection. The source displays its ACR version and the
   load/upload measurements refresh. A file that no longer matches is rejected;
   use **Open OBJ / ACR...** to deliberately load it as a different scene.
4. Choose **Scene > Demo scene > Load mock**, then open the saved ACR through the file picker. Confirm
   the counts return from 16 vertices / 16 triangles / 4 clusters to
   2,012 / 3,936 / 117. A normal Open fits the camera to the newly loaded model.
5. The sections collapse and the panel scrolls when needed. **Last verified save**
   reports the output size, CPU save and verification-reload times. Saving does
   not change the current source or its load/upload timings; replacing the scene
   clears the previous save record. Invalid input keeps the valid scene and metrics.

| Display | What it measures |
| --- | --- |
| Source file | Actual OBJ/ACR bytes on disk; N/A for generated Mock/Cube. |
| GPU payload | Shared vertices, render indices and cluster records for all levels, plus hierarchy records. |
| Resident allocation | Vulkan allocation sizes for the mesh and cluster buffers, including alignment. Excludes staging, UI and framebuffer allocations. |
| CPU prepare | File read + validation; OBJ also includes CPU clustering. Generated scenes include creation + validation. Excludes framing/upload. |
| CPU upload + wait | Upload validation/packing, buffer allocation, staging, submission, synchronization/wait and descriptor update. |
| GPU copies | GPU timestamp interval around the two buffer copies. N/A if the selected queue does not support timestamps. |

For the original single-level Suzanne v1 cache, the ACR file is **172,576 bytes** and the GPU payload is
**117,232 bytes** (64,384 vertices + 47,232 indices + 5,616 cluster records).
The file additionally retains the original indices, position IDs and header.
The fixed Mock uses a 1,216-byte ACR file and an 896-byte GPU payload.

Timings are **last-operation diagnostic samples**, not a performance benchmark.
CPU upload includes waiting/driver work and is not a GPU measurement. GPU queries
check queue support, use the device timestamp period and handle counter wrap;
see the [Khronos timestamp guide](https://docs.vulkan.org/samples/latest/samples/api/timestamp_queries/README.html).
Debug readback and the save/reload equality comparison are excluded from upload
timings. Use the [repeated measurement workflow](#m2-part-2-repeated-measurement) for Release samples with warmup.

## M2 Part 2: test inputs and selection preview

Use **Scene > Demo scene** to switch inputs, or launch one directly:

```powershell
.\build\Debug\AdaptiveClusterRendering.exe --fixture triangle
.\build\Debug\AdaptiveClusterRendering.exe --fixture plane
.\build\Debug\AdaptiveClusterRendering.exe --fixture grid --cluster-colors --bounds
```

| Fixture | Vertices | Triangles | Fixed clusters | Purpose |
| --- | ---: | ---: | ---: | --- |
| Single triangle | 3 | 1 | 1 | Known positions, winding and later subdivision counts |
| Continuous UV plane | 4 | 2 | 1 | One shared diagonal with matching position/UV/normal endpoints |
| Fixed cluster grid (2 x 2) | 9 | 8 | 4 | Two triangles per patch; shared edges across cluster boundaries |

These are authored **source** meshes. All lie in the XY plane at `z=0`, have
unit `+Z` normals and counterclockwise triangle winding when viewed from `+Z`.
Their continuous UV mapping is `u=(x+1)/2`, `v=(y+1)/2`, with no seams or per-patch
UV resets. Grid IDs are `0,1` along the bottom row and `2,3` along the top row.
The original tetrahedron Mock remains available for storage checks.

For a quick demonstration:

1. Load the grid and enable **Cluster colors** and **Cluster bounds**. With
   **All clusters**, four patches and eight triangles are selected.
2. Change **Scene > Cluster inspection > Manual selection** to **First + last cluster**. Only source IDs
   **0 and 3** remain visible, with **2 selected clusters / 4 triangles**. Colors
   still use the original IDs. Bounds are restricted to the selected geometry.
3. Select **Empty list**. Selected counts become zero and both mesh and bounds
   disappear. The full source remains resident, so its storage counts stay the same.
4. Return to All, or switch test scenes (a new source resets selection to All).
   With only one cluster, First + last selects it once. Save/reload works for all
   fixtures through the unchanged ACR v1 format. Saving always stores the full
   source; **Reload saved ACR** retains the current preview selection and camera.

The C++ selection API additionally accepts arbitrary ordered, unique cluster IDs;
the UI exposes four fixed presets, including **Last + first cluster** for order checks. Duplicate, out-of-range, ambiguous All-plus-ID
requests, and stale scene revisions are rejected. Source revisions advance only
after a successful upload, including reloading identical geometry. Selection
resolution is CPU-controlled; the compute copy path below consumes the resolved selection.

The [fixture reference and validation record](docs/milestone-2-part2-contract.md#12-step-3-fixtures-and-selection)
lists expected source counts, sizes and subdivision reference values.

## M2 Part 2: compute copy-through (Step 4)

Enable **Surface > GPU subdivision** with **Segments per edge = 1**, or launch:

```powershell
.\build\Debug\AdaptiveClusterRendering.exe --fixture grid --compute-copy --cluster-colors --bounds --verify-gpu
```

The compute shader reads the selected triangles from the resident source and
writes **independent vertex/index buffers**, which the graphics pipeline draws.
This is the `n=1`, zero-displacement foundation: the surface should look identical
to the source. Each source triangle emits three unshared vertices and three
sequential uint32 indices. Positions, normals, UVs and winding are preserved.
For example, the grid has 9 source vertices / 8 triangles, but its generated copy
has **24 vertices / 8 triangles**. Additional vertices here do not mean subdivision.

Quick demonstration:

1. At n=1, toggle **GPU subdivision** off/on. Compare the same surface and cluster colors;
   the panel identifies source buffers versus GPU output and shows output counts.
2. Choose **First + last cluster**: the grid emits **12 vertices / 4 triangles**
   for source IDs 0,3. **Last + first** changes the output order to IDs 3,0 while
   preserving each patch's color. **Empty list** emits zero geometry and no bounds.
3. Rotate or zoom the camera. The output generation number and last-generation
   timings stay unchanged. **Regenerate output** increments the number once.
4. Switch scenes or reload a saved ACR. The output is regenerated for the new
   source revision. ACR saving continues to store the complete, unchanged source.

The panel separates output/work-list allocation bytes, CPU generation + wait,
and the GPU dispatch timestamp interval. These are last-operation samples,
excluding readback, not benchmark results. Empty output has no dispatch/time;
unsupported timestamps display N/A. Source storage statistics exclude compute
resources. The output allocation budget is 128 MiB; storage-range/count/dispatch
limits are checked before generation. An excessive request reports an error and
returns to source drawing. Changing the selection or source invalidates cached
output; repeated selections and source/output toggles reuse it.

Clustering, work-list preparation and draw submission are still CPU-controlled.
The GPU generates the copy and renders it. Step 5 below extends this path with
uniform subdivision and wireframe; Step 6 adds height displacement.
See the [compute interface and verification record](docs/milestone-2-part2-contract.md#13-step-4-compute-copy-through).

## M2 Part 2: uniform GPU subdivision (Step 5)

```powershell
.\build\Debug\AdaptiveClusterRendering.exe --fixture triangle --subdivision 4 --wireframe --verify-gpu
.\build\Debug\AdaptiveClusterRendering.exe --fixture grid --subdivision 8 --wireframe --cluster-colors
```

**GPU subdivision** enables generated geometry; **Segments per edge (n)** selects
1, 2, 4 or 8. Each selected source triangle becomes `n*n` triangles. This is linear
subdivision: with **Height displacement** off, positions stay on the original
surface, while UVs and normals are interpolated. It does not smooth the shape. The
fragment shader normalizes the interpolated normal for lighting, matching the
source shading field. Original corner attributes remain bit-identical.

| Selected source | n=1 triangles | n=2 | n=4 | n=8 |
| --- | ---: | ---: | ---: | ---: |
| Single triangle | 1 | 4 | 16 | 64 |
| Plane | 2 | 8 | 32 | 128 |
| Grid, All | 8 | 32 | 128 | 512 |
| Grid, First + last | 4 | 16 | 64 | 256 |
| Empty | 0 | 0 | 0 | 0 |

Suggested demonstration:

1. Load **Single triangle**, enable **GPU subdivision** and **Wireframe**, then
   switch n through 1/2/4/8. The interior edges become denser; check the table above
   against **Output triangles**. Vertices are unshared: output vertices = 3 × triangles.
2. Load **Fixed cluster grid (2 x 2)** and enable **Cluster colors**. At n=8, All
   yields 512 triangles; First + last yields 256 with original IDs 0,3. Empty yields
   zero geometry at every subdivision level.
3. Toggle **GPU subdivision** off/on with the same selection and camera. Wireframe
   compares the original topology to the generated topology; filled rendering
   retains the same surface. Camera and wireframe changes reuse the output.
4. Change n: the generation number and counts update once. Output allocation scales
   with n squared, but the CPU-authored source triangle address list stays the same
   size. **Regenerate output** forces a fresh timing sample without changing n.

Wireframe is a line rendering mode, not a filled overlay, and is available only
when the selected GPU supports `fillModeNonSolid`. Unsupported devices keep normal
filled rendering and show a disabled control. All selected clusters use the same
n; per-cluster adaptive levels/stitching are not implemented. Shared indexed edges
use the same interpolation order in both directions to avoid inconsistent samples.
UV/normal splits remain source seams; this is not a general displaced seam repair.

Generation remains synchronous and cached, with a 128 MiB output budget. ACR stores
the unchanged source, not the generated mesh. For implementation and validation
details, see [Step 5](docs/milestone-2-part2-contract.md#14-step-5-uniform-gpu-subdivision).

## M2 Part 2: GPU height-map displacement (Step 6)

```powershell
.\build\Debug\AdaptiveClusterRendering.exe --fixture grid --subdivision 8 --displacement 0.35 --bounds
.\build\Debug\AdaptiveClusterRendering.exe --fixture plane --subdivision 8 --displacement 0.25 --heightmap assets/heightmaps/ramp-u.pgm --verify-gpu
```

The compute shader samples a real GPU height image and moves each subdivided
vertex along its interpolated, normalized source normal:
`offset = normal * amplitude * (2 * height - 1)`.
The result is changed geometry, visible in wireframe and silhouette. CPU code loads
the image, supplies settings/work lists and schedules generation; CPU reference
geometry is used only for verification. Output counts remain `T*n*n` triangles.

Suggested demonstration:

1. Start with **Fixed cluster grid**, **GPU subdivision** on, n=8 and **Height
   displacement** on. **Waves** is the default image. Move **Amplitude** between
   0 and about 0.5 and observe the actual surface and lighting change.
2. Compare **Mound**, **Ramp U** and **Ramp V**. **Neutral (0.5)**, amplitude zero
   and disabling displacement restore the original surface and shading. Black/white
   offset the surface by minus/plus amplitude along its source normals.
3. Enable **Wireframe** and **Cluster colors** to see subdivisions and the original
   cluster IDs. Continuous shared indexed edges stay connected across clusters.
   **Cluster bounds** displays conservative boxes that grow with displacement.
4. Toggle **GPU subdivision** off/on to compare the original selected mesh with
   the generated result. Zoom and wireframe/source comparison reuse cached output;
   changing the height map, amplitude or displacement toggle regenerates it.
5. **Open height map (.pgm)...** opens beside the bundled `ramp-u.pgm` example on
   Windows. Select it to tilt the plane using a loaded image. File selection can be
   cancelled without changing the current image or output. Open **Stats > GPU output**
   for the generated counts, bounds padding, allocations and timings.

Height files are **PGM P2 (ASCII) or P5 (binary, 8/16 bit)**, one image per file,
maximum 2048x2048. Samples are interpreted as linear numbers, without gamma
conversion. PNG/JPEG are not supported in this stage. Row zero maps to v=0 (the
bottom of the XY fixture); there is no automatic image flip. UVs clamp to [0,1].
Manual bilinear sampling maps UV endpoints to the first/last texel centers at mip
level zero. Use the Neutral preset for exactly 0.5: an 8-bit 0..255 image cannot
represent exactly 0.5. The example P2 uses maximum 8 and middle sample 4.

The UI amplitude range is 0..1 model units; the CLI/API accept finite 0..1000.
`--displacement` also enables GPU geometry, while `--heightmap` only chooses an
image. With a nonzero displacement bound, generated triangles use **flat geometric
normals**, including locally neutral portions of a nonneutral image. This exposes
the new shape in the existing lighting; it is not a smooth-normal reconstruction.
Near-zero source normals contribute no displacement; degenerate faces fall back
to the source direction or +Z. Imported UV/normal seams can still open under
displacement; general seam repair and adaptive levels are outside this stage.

Height images have their own allocation counter, separate from the 128 MiB
generated-output budget and source storage counters. GPU generation timing now
includes sampling and normal calculation but excludes image upload and debug
readback. The height map/settings and generated geometry are runtime state: ACR v1
still saves the unchanged source. See the [Step 6 interface and validation record](docs/milestone-2-part2-contract.md#15-step-6-gpu-height-map-displacement).

## M2 Part 2: repeated measurement

Open **Stats > Performance measurement** to run 10 warmup + 50 measured repetitions by
default. The viewer locks the scene, view and settings, forces a new generation
on every run, and exports JSON with individual samples, median, p95 and range.
Cancel or resize discards the incomplete batch. The UI labels the completed
result as a fixed snapshot, independent of subsequent changes to controls.

```powershell
cmake --build --preset release --parallel 1
.\build\Release\AdaptiveClusterRendering.exe --fixture grid --subdivision 8 --displacement 0.25 --benchmark build/benchmarks/grid.json
.\scripts\Measure-M2.ps1
```

CLI measurement runs hidden and excludes UI drawing; interactive measurement
includes it. GPU generation, GPU render-pass time and CPU generation-plus-wait
latency have separate scopes. Neither GPU metric is full frame time/FPS, and no
CPU/GPU speedup is claimed. [The guide](docs/milestone-2-part2-demo.md#reproducible-measurement)
defines the timing boundaries, optional counts/selection, export fields and
current reallocation/synchronization costs. Ordinary interaction still caches output.

## Validation

```powershell
cmake --preset windows-vs2022 -DACR_ENABLE_GPU_TESTS=ON
cmake --build --preset debug
ctest --test-dir build -C Debug --output-on-failure
```

CPU tests cover the fixed mock, lossless roundtrip, real OBJ geometry, deterministic baseline/QEM-guided clustering, invalid scene data, and malformed/version-mismatched files. The current QEM experiment retains baseline cluster membership, ranges and bounds, sorting triangles only within each cluster; tests also retain determinism, coverage and roundtrip checks. Optional GPU tests run hidden windows, verify uploads, replace real/mock scenes, and submit Draw All, cluster color, all-bounds and selected-bounds draws. The GPU tests need a working Vulkan window system. They fail on validation warnings/errors; use **Debug** and install the Khronos validation layer to obtain that coverage. `ACR_ENABLE_GPU_TESTS` is off by default on other machines.

The `scene-storage-stats` case checks source metadata, exact file/payload sizes,
CPU timings, field-level roundtrip comparison and failed-load preservation.
The scene/geometry smoke tests save to disk, switch to Mock, reload and compare the original
scene, check resident sizes/timestamp availability, and confirm that a rejected
upload preserves the previous GPU scene and measurements. These tests can be run
individually with `--smoke-roundtrip build/test-roundtrip.acr` plus an input path
or `--mock`; the output parent directory must already exist.

Four `fixture-*` CPU cases check authored counts, UV mapping, unit normals, winding,
area, shared-edge topology, cross-cluster continuity, ACR roundtrips and selection
rejection rules. Three additional `gpu-fixture-*` cases exercise fixture upload and
save/mock/reload. All six GPU cases also verify all/partial/empty preview draw counts,
bounds filtering and scene-revision invalidation. These checks validate source
inputs and the original renderer. Separate subdivision tests cover the generated geometry.

Two `geometry-copy-*` CPU tests cover compaction, selection order/cache keys,
empty output, checked counts, capacity rejection and dispatch boundaries.
Six `gpu-copy-*` tests add actual compute/readback/draw checks for Mock, OBJ, ACR
and all three fixtures. They compare every generated position/normal/UV bit and
index against the source, verify source immutability, and exercise reversed/empty
selection, cache reuse, forced regeneration, rejected requests, identical-source
replacement and source/output drawing.

Two `subdivision-*` CPU cases check all level/count/budget rules and deliberately
corrupt hand-authored output to exercise the independent geometry verifier. Six
`gpu-subdivision-*` cases exercise all four levels, actual readback and drawing,
setting changes, wireframe, partial/reversed/empty selection, and source replacement.
Readback checks double-precision barycentric position/UV/normal references, original
corners, shared indexed-edge equality, area, winding and indices for the
zero-displacement baseline.

Three `displacement-*` CPU tests cover bilinear/clamped/one-pixel sampling, height
formats and malformed files, neutral bounds, normalized directions and corrupted
displaced output. Four `gpu-displacement-*` cases (triangle, plane, grid and OBJ)
exercise actual height-image uploads, all levels, neutral/off/zero amplitude,
black/white/ramps/waves/mound, file reload, partial/reversed/empty selection, zero
source normals, rejected settings and resource/cache transitions. Every generated
vertex is read back against a double-precision reference; normals are checked
against the generated faces, shared indexed-edge positions/UVs are compared
bit-for-bit, expanded AABBs are checked, and source buffers are verified unchanged.

Three `benchmark-*` CPU cases check statistics, warmup exclusion, cache/query
rejection, cancellation and safe report validation. Four `gpu-benchmark-*` cases
parse the actual JSON for all/partial/empty/source runs, verify sample counts,
selection order and generation revisions, and perform readbacks outside the batch.

The render integration test samples camera depth. The replay test compares the restored camera, fixed cluster IDs and draw counts. The shadow tests compare entire depth maps for all casters and filtered casters, including a patch outside the camera view.

Run the full suite with Vulkan synchronization validation in Debug. Run it again with the Release binary for the configuration used in demos.

```powershell
$env:VK_LAYER_VALIDATE_SYNC = '1'
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build -C Release --output-on-failure
```

For the CPU tests only: `ctest --test-dir build -C Debug -LE gpu --output-on-failure`. The cached-scene preprocessing test is also CPU-only. No FPS or GPU speedup is asserted by these checks.

## Source layout

| File | Responsibility |
| --- | --- |
| `src/ObjLoader.cpp`, `src/MeshSource.h` | Read OBJ positions, normals and UVs into an indexed mesh. Supply a face normal for corners that lack one. |
| `src/ClusterBuilder.cpp` | Group edge-connected triangles on the CPU. Each cluster has at most 64 triangles, a contiguous index range, and a model-space bounding box. |
| `src/QemClusterBuilder.*` | Experimental QEM-based ordering within baseline clusters and internal-edge score measurement. Geometry is unchanged; this library/test path is not selected by the viewer. |
| `src/MeshHierarchy.*` | Address and validate previously stored ACR levels, bounds and child links. |
| `src/SceneData.*` | Shared scene handoff, fixed mock, strict validation, OBJ/ACR loading, and versioned persistence. |
| `src/SceneDiagnostics.*` | Runtime source/file summaries, CPU preparation timing, payload byte counts and exact save/reload verification. Leaves ACR v1 unchanged. |
| `src/TessellationFixtures.*` | Authored triangle, continuous-UV plane and four-cluster grid for module 07 correctness checks. |
| `src/ClusterSelection.*` | Revision-bound All/Explicit selection, ordered source ranges and rejection of duplicate, invalid or stale IDs. |
| `src/GeometryGenerationPlan.*` | Check subdivision sizing and expand fixed ranges into triangle addresses. The shadow pass can supply device-generated work. |
| `src/HeightField.*` | Linear height presets, PGM loading, displacement settings/bounds and CPU reference sampling. |
| `src/ComputeGeometry.*`, `shaders/generate_geometry.comp` | GPU subdivision/displacement, height-image and generated-buffer ownership, cache identity, barriers and generation timings. |
| `src/GeometryValidation.*` | Independent CPU reference checks for GPU readback; never generates the displayed geometry. |
| `src/Benchmark.*`, `scripts/Measure-M2.ps1` | Fixed-input measurement state, statistics/JSON export, and a sequential reproducible case matrix. |
| `src/ClusterGpu.h`, `src/SceneGpuData.h` | Explicit 48-byte std430 cluster records and the non-owning GPU scene view. |
| `src/VulkanRenderer.cpp`, `shaders/` | Upload geometry and cluster descriptors; depth-tested mesh and SSBO-driven bounds. In cluster color mode, draw each index range with its own color. |
| `src/Viewer.cpp`, `src/OrbitCamera.cpp` | Manage model loading, camera controls, and the Model panel. |
| `src/VulkanRendererShadowCasters.cpp`, `shaders/select_shadow_casters.comp` | Filter padded cluster bounds against the directional light and compact caster draws and subdivision work. |
| `src/VulkanRendererShadow.cpp`, `src/RenderPlanning.cpp` | Directional shadow map, fitted light projection, coverage control and depth readback. |
| `src/ViewConfig.*`, `shaders/camera_depth.frag` | Save and replay viewer configurations, and display the stored main camera depth. |

The main pass and shadow pass can draw different ordered cluster lists. Both consume source or displaced geometry.

## Credits

The viewer uses [GLFW](https://github.com/glfw/glfw) for windowing (zlib/libpng), [GLM](https://github.com/g-truc/glm) for math (MIT or Happy Bunny License), [TinyObjLoader](https://github.com/tinyobjloader/tinyobjloader) for OBJ import (MIT), and [Dear ImGui](https://github.com/ocornut/imgui) for the Model panel (MIT). Versions are pinned in `CMakeLists.txt`. The [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) provides the Vulkan development tools.

The bundled OBJ is adapted from the [Khronos Suzanne sample](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Suzanne), credited to UX3D and Norbert Nopper (2017) under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/).
