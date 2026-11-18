# Milestone 2 acceptance and demo

The local engineering checks cover modules 01–08 of the group plan. The real-model test follows Suzanne from OBJ preprocessing through ACR v2 reload, GPU LOD and visibility selection, displaced subdivision, main drawing and directional shadows. In a separate four-cluster scene, an object outside the camera casts a shadow onto the center of the image.

## One-minute shadow demo

From the repository root in a Release build, launch the saved view.

```powershell
.\build\Release\AdaptiveClusterRendering.exe --view-config assets/views/m2-shadow-casters.acrview
```

1. Open **Rendering and shadows**. Point out the dark patch on the red receiver. Its caster is above the camera's field of view. The two small side markers are visible to the camera.
2. Keep **Shadow coverage** at **0.30**. Toggle **Cull casters in light view** off, then on. The shadow draw falls from **4 clusters / 32 triangles** to **2 clusters / 16 triangles**. The camera still draws 3 clusters / 24 triangles and the dark patch stays in place.
3. Turn **Shadows** off, then on. The dark patch disappears and returns. Choose **Shadow map (light view)** to inspect the depth stored by the light. The **Light from left** and **Light from right** buttons change the light direction. Use **Open view...** to restore the starting pose and settings.

The light's coverage slider changes the width and height of a centered orthographic projection. At **1.00** it fits the complete scene. A smaller value concentrates the map on the center and leaves receivers outside it unshadowed. Keep coverage, camera, light direction and subdivision fixed when comparing culling on and off.

For the complete pipeline on a real mesh, open the second preset. In **Mesh hierarchy**, change **Allowed screen error** to see regions choose different levels. Restore the preset before comparing the counts below. Both presets can also be opened with **Open view...**.

```powershell
.\build\Release\AdaptiveClusterRendering.exe --view-config assets/views/m2-suzanne.acrview
```

## Module coverage

| Module | Implemented behavior | Acceptance evidence |
| --- | --- | --- |
| 01 · Mesh building | Groups neighboring clusters, simplifies with boundary protection and reclusters. Each hierarchy node stores bounds, error and its child links. | Suzanne has six resident levels, from 3,936 triangles at L0 to 230 at L5. `scene-hierarchy` checks the hierarchy. |
| 02 · Geometry storage | Saves and loads the multilevel mesh in ACR v2 and uploads it as fully resident buffers. The loader also accepts flat ACR v1. | `gpu-hierarchy-roundtrip` checks resident data and draws every level. The end-to-end run repeats the same selection and draws from a saved ACR. |
| 03 · LOD selection | GPU screen-error traversal chooses detail per region and supplies draw commands and subdivision work. | `gpu-lod-camera-depth` compares the GPU result with a CPU reference at different camera distances. The Suzanne preset selects 78 clusters across four levels. |
| 04 · Visibility culling | The camera applies frustum, hierarchy and optional Hi-Z occlusion tests with bounds expanded for displacement. | `gpu-visibility-pipeline` checks a hidden patch, exact camera depth and each level of the depth pyramid. |
| 05 · Rasterization | Draws the selected source or subdivided geometry. Supports all, partial and empty selections, wireframe and fixed-view replay. | `gpu-render-integration` and `gpu-visibility-report` check submitted counts and empty draws. |
| 06 · Visibility and material | Provides shaded and camera-depth views, sampled camera depth for module 04, timing reports, panel hiding and saved views. | `gpu-lod-camera-depth` checks depth sampling and resizing. `gpu-view-replay` checks the restored view and selection. |
| 07 · GPU tessellation and displacement | Subdivides selected triangles at a fixed level of 1, 2, 4 or 8 segments per edge. Samples a height map and supplies displaced geometry to the renderer. | The subdivision and displacement GPU tests compare generated vertices, indices, shared edges and bounds with CPU references. |
| 08 · Shadow rendering | The light independently filters the manual or GPU LOD candidates with its own GPU frustum test. It draws source or displaced geometry into a 1024 × 1024 directional shadow map, then the main pass samples that map. | `gpu-shadow-caster-pipeline` checks complete depth images and the off-camera caster. `gpu-shadow-caster-report` checks public viewer options, replay and the combined OBJ/ACR run. |

Camera Hi-Z uses a previous solid frame with matching camera, viewport, geometry and displacement settings. After an input change, the camera can use frustum tests until it has matching depth again. The light starts from the LOD candidate list before camera culling and needs only its own frustum. Both views keep separate GPU draw commands and subdivision work. The CPU reads back compact IDs and counts for output sizing, debug colors and reporting.

## Fixed-view results

Measured on an NVIDIA GeForce RTX 4070 Laptop GPU in MSVC Release at 960 × 640, with 5 warmup frames and 30 measured frames per row. Both presets use camera Hi-Z, 2 segments per edge and displacement amplitude 0.08. Suzanne uses a 96-pixel LOD threshold. Only the light-culling switch changes between the two runs for each scene.

| Scene / light coverage | Light culling | Light clusters | Shadow triangles | Camera triangles | GPU caster test (ms, median) | GPU shadow draw (ms, median) | CPU frame (ms, median) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Off-camera caster / 0.30 | Off | 4 / 4 | 32 | 24 | — | 0.0099 | 3.53 |
| Off-camera caster / 0.30 | On | 2 / 4 | 16 | 24 | 0.0096 | 0.0091 | 5.04 |
| Suzanne / 0.45 | Off | 78 / 78 | 7,288 | 7,288 | — | 0.0240 | 5.32 |
| Suzanne / 0.45 | On | 75 / 78 | 7,244 | 7,288 | 0.0124 | 0.0249 | 7.36 |

The camera and light matrices and camera selections match within each pair. On these two scenes the selector reduces submitted geometry and increases measured CPU frame time. The benchmark repeats selection and main geometry generation every measured frame. Enabling light culling adds a synchronized dispatch and count readback. Its median CPU time including the wait was 1.26 ms on the fixture and 1.36 ms on Suzanne. Interactive frames can reuse a light selection while its inputs are stable.

To reproduce the four reports, run the following from the repository root.

```powershell
foreach ($scene in 'shadow-casters', 'suzanne') {
    foreach ($mode in 'all', 'frustum') {
        .\build\Release\AdaptiveClusterRendering.exe --view-config "assets/views/m2-$scene.acrview" --shadow-cull $mode --verify-gpu --benchmark "build/m2-final-$scene-$mode.json" --benchmark-warmup 5 --benchmark-samples 30
    }
}
```

## Verification

Both Debug and Release passed **61/61 tests**, including **37 Vulkan GPU tests** per build. Synchronization validation was enabled for the Debug run. The shadow test compared the entire camera and light depth images with culling on and off. It then removed just the off-camera caster and confirmed that the light texel at the visible receiver changed from caster depth to receiver depth. It also covers fully rejected lists, different camera/light lists of equal length, displaced bounds, a manual coarse level and mixed LOD. The saved-asset test compares camera/light matrices, selected IDs and submitted geometry after reloading preprocessed Suzanne.

```powershell
cmake --preset windows-vs2022 -DACR_ENABLE_GPU_TESTS=ON
cmake --build --preset debug
cmake --build --preset release
$env:VK_LAYER_VALIDATE_SYNC = '1'
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build -C Release --output-on-failure
```
