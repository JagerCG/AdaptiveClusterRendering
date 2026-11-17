# M2 Part 2 demo and measurement guide

Updated 2026-11-14. Member 2: module 02 storage and module 07 GPU subdivision /
height displacement. This is the standalone M2 deliverable; integration with the
other members' hierarchy, visibility and shadow paths is still pending.

## Start the demo

From the repository root:

```powershell
cmake --build --preset release --parallel 1
.\build\Release\AdaptiveClusterRendering.exe --fixture grid --subdivision 8 --displacement 0.25 --cluster-colors --bounds
```

Maximize the window. Collapse sections you are not discussing, or use the panel's
scrollbar in a small window. **Test inputs** also offers a single triangle and a
continuous-UV plane. Use the plane/grid for displacement: the original Mock scene
has zero UVs and is intended for storage checks.

## A 60-90 second walkthrough

| Time | Action | What the audience should see / hear |
| --- | --- | --- |
| 0-15 s | Enable Wireframe. Set n to 1, 2, 4, then 8. | The four fixed clusters have 8 source triangles. The GPU output has 8, 32, 128 and 512 triangles. Source counts stay unchanged. n is segments per edge, so output grows as n squared. |
| 15-30 s | Disable Wireframe; toggle Height displacement, change amplitude, then switch Waves to Mound. | Actual positions and face normals change on the GPU. Bounds expand conservatively with amplitude. This is geometry, not just a texture or color change. |
| 30-45 s | Select First + last, then Empty, then All. Toggle GPU subdivision off/on. | Fixed source cluster IDs determine the work. Empty means zero output. Turning generation off draws the original flat source; turning it back on reuses the cached output when settings match. These IDs are a stand-in for future visibility output. |
| 45-60 s | Expand Performance measurement and click Measure with 10 warmup / 50 measured runs. | Parameters and camera lock during measurement. Show GPU generation and render medians/p95 plus the output allocation. Explain that CPU generate + wait includes allocation and synchronization. |
| Extra 20-30 s | Open Suzanne; Save + verify ACR, Load mock, Reload saved ACR. Expand Storage. | Geometry, partition and source IDs survive disk roundtrip exactly. ACR loads the existing partition. File bytes, resident payload/allocation and startup timings are separate quantities. |

For a strict one-minute slot, demonstrate the grid and show the storage result in
a prepared screenshot or slide. **Regenerate output** performs one generation;
**Measure** performs a warmup followed by repeated work. Orbit/zoom and Wireframe
reuse the geometry in ordinary interactive mode, so their displayed last-generation
time should not be described as a per-frame cost.

## Reproducible measurement

The UI defaults to 10 warmup and 50 measured runs. Counts can be changed before
starting (warmup 1-1000; measured 2-10000). Cancel restores the controls without
exporting partial data. A framebuffer resize cancels the run because the render
workload has changed. Completed reports remain labelled as a fixed snapshot even
after you change settings. UI reports are written beside the executable under
`benchmarks/`; the panel shows the full path.

For comparable render measurements, use the hidden CLI mode, which renders the
same scene without drawing ImGui:

```powershell
.\build\Release\AdaptiveClusterRendering.exe --fixture grid --subdivision 8 --displacement 0.25 --benchmark build/benchmarks/grid.json
.\build\Release\AdaptiveClusterRendering.exe assets/suzanne.obj --subdivision 8 --displacement 0.25 --benchmark build/benchmarks/suzanne.json --benchmark-warmup 10 --benchmark-samples 50
```

Optional selection: `--selection all|first-last|empty|last-first`. Omitting all
compute-enabling switches measures the source rendering path; CPU/GPU generation
fields are then null. An empty generated selection has CPU setup time, no dispatch
time, and a render interval that still includes the clear (and UI in interactive
mode). Unsupported GPU timestamps are null, never fabricated zeroes.

The seven-case matrix used for the recorded results is reproducible with:

```powershell
.\scripts\Measure-M2.ps1
# Optional: -Warmup 20 -Samples 100 -OutputDirectory build/benchmarks/my-run
```

It runs sequentially, writes one JSON per case, and writes `summary.csv`. It uses
Release by default; build before running it. For a submission-quality comparison,
use the same GPU/build/scene/selection/camera/resolution/settings and an idle GPU.
Keep Debug validation runs separate from Release performance runs.

| Metric | Included | Excluded / interpretation |
| --- | --- | --- |
| CPU generate + wait | CPU planning, output/worklist reallocation, worklist upload, recording, submit, queue wait and result publication | Prior frame-fence wait, height upload, correctness readback. This is elapsed CPU-side latency including waits, not CPU-only algorithm time. |
| GPU generate | GPU timestamps bracketing the compute dispatch, including subdivision, active height sampling and normal calculation | Host allocation/recording/waits, height upload, readback, mesh drawing. |
| GPU render | Timestamps around the render pass: clear, mesh, optional bounds; UI drawing for interactive measurements only | Generation, acquisition/presentation waits and CPU work. This is not full frame time or FPS. |
| Startup timings | Last file preparation/upload operation; JSON fields start with `startup_` | Not part of the repeated-sample statistics. Save/reload checks remain in Storage. |

Measured render submissions wait for the swapchain image at TOP_OF_PIPE before
the first timestamp. Ordinary rendering retains its original wait stage. The
single-frame benchmark serializes generation and rendering; it does not measure
an overlapped/async pipeline. Current forced generation reallocates output each
run, so its CPU latency is not a preallocated steady-state kernel benchmark.

Only successfully submitted runs count. Warmup samples are discarded. Reports
contain all measured samples, generation revisions, min/median/nearest-rank p95/
max/mean, GPU/build/validation metadata, camera matrix, source and height identity,
selected IDs, draw counts and memory sizes. Generation revisions must increase:
a cache hit cannot count as a new measured generation. `--verify-gpu` checks
correctness before and after the batch, outside the measured samples.

See the [recorded Release measurements](milestone-2-part2-performance.md).

## Verification and handoff

On 2026-11-14, **48/48 Debug CTest cases passed**, including 26 GPU cases, with
Khronos synchronization validation and no Vulkan validation warnings/errors.
Seven benchmark-focused tests also passed in Release. CPU coverage checks warmup
exclusion, statistics, cancellation, cached-generation rejection, missing queries,
JSON escaping and incomplete-export rejection. The GPU benchmark tests independently
parse reports for all/reversed/empty/source cases and verify output/source data
before and after repeated work.

Interactive checks covered complete export, count editing, locked controls,
manual cancellation, resize cancellation, restored controls and a scrollable small
window. A cancelled/resize-invalidated run created no extra report. Keyboard entry
worked; clipboard pasting in the automation session encountered a Windows clipboard
access error, so paste support was not verified.

| Consumer | Available now | Agreement/work still needed |
| --- | --- | --- |
| Module 01 -> 02 | Validated source scene, ACR v1, upload diagnostics and exact roundtrip | Define and version hierarchy, coarse/fine meshes, geometric error and new bounds before extending the cache format. |
| Modules 03/04 -> 07 | Revision-bound ordered All/Explicit source cluster selection; empty selection supported | Real GPU-produced selection/count buffers, work expansion, capacity policy, indirect dispatch and synchronization. The current producer/work list is CPU-authored. |
| Module 07 -> 05 | Borrowed generated vertex/index view, source-ID ranges, counts, generation identity and bounds padding | Coordinate renderer ownership/lifetime, transform handling and final pass timing; match displaced geometry in shadows. |
| Module 07 -> 04 | `displacementBound(height, settings)` before generation; conservative object-space AABB padding | Apply the same bound before visibility tests, with appropriate transforms. Displaying padded boxes alone does not implement culling. |

The implementation uses uniform n and flat displaced face normals. Per-cluster
adaptive levels, arbitrary UV/normal seam repair, smooth displaced normals, demand
paging, GPU LOD/culling and shadows are not completed by this Part 2 milestone.
