# M2 Part 2 and rendering integration

The [01/02 hierarchy handoff](../README.md#m2-mesh-hierarchy-and-acr-v2) implements mesh preprocessing and resident storage. The [03/06 handoff](../README.md#m2-gpu-lod-and-camera-depth) adds GPU-selected geometry, saved views and the sampled main camera depth for module 04. The notes below describe the earlier integration checkpoint.

Updated 2026-11-16. This integrates the standalone Part 2 checkpoint `f5e9e8d`
with member 3's PR #3, upstream `5224108`. No GPU LOD or hierarchy completion is
implied by this integration.

## What now works together

- The existing storage/ACR workflow, generated geometry, height maps, cache,
  selection identities, wireframe and benchmark exporter are retained.
- Shaded, camera-depth and light-depth views consume the selected source or
  generated geometry. Generated shadow draws use generated index ranges/counts,
  including partial/reversed selections and explicitly empty selections.
- Directional shadows use a 1024 x 1024 D32 depth image with stored contents,
  point sampling and a 3 x 3 manual comparison filter. The receiver uses exactly
  the matrix used to render the shadow map. The matrix fits source bounds plus
  conservative displacement, independently of the camera. Receiver bias is a
  working control. Lighting uses the same direction as the shadow projection.
- A real shadow-map display shows the full light depth image, preserving its
  square aspect ratio. Both depth displays use near = bright, far/empty = dark.
- Optional CPU frustum/distance previews filter main mesh and bounds draws,
  regardless of cluster color mode. They preserve source IDs and empty results.
  Shadows retain all authored input casters, including camera-culled objects.
- Timing reports include `gpu_render` (total), `gpu_main`, and `gpu_shadow`.
  A disabled shadow pass has a null shadow timing. Acquiring/presenting the
  swapchain, correctness readbacks and CPU waits remain outside GPU intervals.

The CPU previews filter drawing after generation; they do not reduce generation
work and do not implement GPU LOD selection, Hi-Z or modules 03/04. The upstream
placeholder LOD switch was replaced with explicitly named preview controls rather
than labeling a distance heuristic as actual hierarchy-based LOD.

## Build and quick demo

From the repository root, in PowerShell:

```powershell
cmake --preset windows-vs2022 -DACR_ENABLE_GPU_TESTS=ON
cmake --build --preset debug --parallel 1
.\build\Debug\AdaptiveClusterRendering.exe --fixture shadow --subdivision 8 --displacement 0.35 --shadows
```

Both build systems are available. `debug`/`release` use the Visual Studio preset;
`ninja-debug`/`ninja-release` use `windows-ninja`/`windows-ninja-release` in an MSVC
developer environment. Configure the corresponding preset before building it.

The shadow stage has a large receiver and a suspended patch, one authored cluster
each. Its 4 input triangles become 256 at n=8. Waves/Mound keep the receiver neutral
at its constant UV (0,0) while deforming the patch. Arbitrary custom height maps can
also displace the receiver; it is ordinary scene geometry, not a special render pass.

1. Maximize the window. Expand **Rendering and shadows** and choose **Light from
   left**, which separates the patch from its shadow clearly in the initial view.
2. Toggle **Shadows**. The projected dark shape disappears/reappears.
3. Toggle **Height displacement**, change its amplitude, or choose Mound/Waves.
   Compare both the patch silhouette and the shadow. Turn **GPU subdivision** off
   to compare the original flat patch. Use **Wireframe** to inspect generated edges.
4. Switch Shaded -> Depth -> Shadow map. The first depth view follows the camera;
   the shadow map follows the light. Rotate the camera: the directional shadow
   projection and cached generated result remain stable.
5. In **Test inputs**, select **Empty list**: no mesh, bounds or shadow casters
   remain. Restore **All clusters**. On the four-cluster grid, compare ordered
   subsets with cluster colors on and off.
6. Enable CPU frustum preview or set a small positive maximum distance. Main draws
   can become empty without falling back to All; the shadow caster set is unchanged.
   Set distance back to 0 to disable that filter.
7. Expand **Performance measurement**, fix the view/settings, and measure. The
   controls lock while sampling and the JSON records the display, light, bias,
   visibility settings and both main/shadow triangle counts.

CLI equivalents include `--shadows`, `--shadow-bias`, `--depth-view`, `--shadow-map`,
`--frustum-preview`, and `--max-distance`. Example Release report:

```powershell
cmake --build --preset release --parallel 1
.\build\Release\AdaptiveClusterRendering.exe --fixture shadow --subdivision 8 --displacement 0.35 --shadows --benchmark build/shadow-stage.json
```

This is a correctness/integration baseline, not evidence of an overall GPU speedup.
The fixed shadow image and light uniform are additional renderer resources, outside
the existing geometry/height allocation counters. D32 sampled depth and transfer
support are checked at initialization. Model transforms are currently identity.

## Shared interfaces

The renderer owns source, depth and lighting resources; `ComputeGeometry` owns
generated buffers. Both main and shadow passes borrow the same valid source or
generated view. A selected source ID remains a source ID after compact generation;
neither pass may renumber it or fall back to drawing the full source.

Main camera filtering and shadow casting deliberately use different lists. Formal
GPU integration must supply camera/light visibility independently, or use a
conservative caster set; camera-invisible geometry can cast visible shadows.

The graphics push-constant range is **96 bytes**, visible to vertex and fragment
stages: matrix at 0, color at 64, bounds padding at 80, display mode at 84, shadow
flag at 88, comparison bias at 92. Set 0 retains the cluster SSBO; set 1 contains
the sampled shadow depth at binding 0 and an 80-byte light matrix/direction uniform
at binding 1. It is initialized and bound even when shadows are off. This resolves
the old collision between Part 2's bounds padding and PR #3's fragment flags.

One frame fence protects resource replacement and uniform writes. Render-pass
dependencies make shadow depth writes visible to sampling; validation readbacks
restore the sampled layout. Display, light, bias and camera-only changes reuse
generated geometry. Future GPU work lists still need their own dispatch/count and
synchronization contract.

`plannedDisplacementBound()` exposes the conservative bound before generation.
The CPU preview uses the same bound on the generated path. This validates its
meaning without claiming that the future GPU culling consumer has been integrated.

## Remaining team dependencies

| Interface | Still required |
| --- | --- |
| 01 -> 02 | Coarse/fine geometry, hierarchy IDs, errors and bounds; agree a versioned persistent format before extending ACR v1. |
| 03/04 -> 07 | GPU-selected cluster IDs/count, work expansion and capacity/overflow rules, indirect dispatch and producer/consumer synchronization. Current generation inputs are CPU-authored. |
| 07 -> GPU 04 | Apply the planned displacement bound before actual GPU visibility/occlusion tests, including transforms if added. |

The existing QEM experiment now follows PR #3: baseline cluster membership/bounds
are retained and triangles are sorted inside each group. It does not simplify
geometry or produce an LOD hierarchy. Historical Step 1 grouping measurements in
the Part 2 contract describe the earlier algorithm, not this merged version.

## Verification

The Debug suite passed **53/53** checks with Vulkan synchronization validation
enabled. This includes original storage, fixtures, selection, copy, subdivision,
displacement and benchmark checks plus the new render planning/integration tests.
The GPU checks fail on validation warnings as well as errors.

`--render-smoke` exercises source/generated/shadow/depth/wireframe paths, ordered
subsets, empty selections, all-culled main draws, camera/light changes, source
replacement, resize and pass timestamps. Shadow depth is copied back only in this
validation path and compared to independent plane/ray reference depths, including
displaced geometry and an angled light. Neutral subdivision is compared with the
source shadow map within a small raster-edge tolerance.

```powershell
$env:VK_LAYER_VALIDATE_SYNC = '1'
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\AdaptiveClusterRendering.exe --mock --render-smoke
```

Final verification also passed **11/11 Release checks** (render planning, the GPU
integration sequence, benchmark statistics/session/export, and six persisted
report scenarios). The last label/aspect adjustments were followed by successful
Debug render-integration and shadow-map report checks. Release reports retain
separate main/shadow samples; unavailable/disabled timings remain null.

Interactive validation observed shadow toggling, light-direction changes, camera
and light depth displays, complete labels in the narrow control panel, and a
square shadow-map display after maximizing the window. Debug validation output
contained no warnings/errors during these checks.

Local verification logs are in `build/render-integration-debug-tests.log`,
`build/render-integration-release-tests.log`, and `build/render-integration-ui-final.log`.
The build folder is intentionally not committed. Validation was performed on the
NVIDIA GeForce RTX 4070 Laptop GPU with Vulkan SDK 1.4.363.0. Ninja presets were
retained but not exercised on this machine; MSVC Debug and Release were built.
