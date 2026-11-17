# Milestone 2 Part 2 foundation and integration contract

Prepared: 2026-11-13. Scope: member 2, modules 02 and 07.

This is the working contract for developing Part 2 from the synchronized M1
baseline. Sections 10-16 record completed work; the other sections define the
shared interfaces and remaining implementation. Storage diagnostics, fixed source
selections, compute-to-render integration, uniform GPU subdivision and wireframe
comparison, GPU height displacement and repeated performance measurement exist
(Step 7 completed 2026-11-14). The standalone Part 2 M2 demo is ready.
The decisions below are this branch's implementation defaults. Team review of
the shared integration points is still pending.

## 1. Baseline and scope

- Upstream baseline: `1b639be0c8b41d3ea61e0b308b9f2cfeae892eb6` on `main`.
- Development branch: `feat/m2-part2-foundation`.
- Preserve the existing OBJ/ACR/mock viewer, ACR v1 format, scene validation,
  GPU readback verification, and fully resident source geometry.
- The new `buildQemGuidedClusters` library/test path changes triangle grouping
  using QEM edge scores. It does not simplify geometry or build an LOD hierarchy.
  The viewer's default `buildScene` path still uses `buildClusters`.
- QEM grouping scores are not a replacement for a future LOD geometric-error
  bound. That representation must be agreed with module 01.

The team's `Adaptive_Cluster_Rendering_Plan.pdf`, member 2 page, separates the work:

| Module | M2 independent deliverable | Later integration before M3 |
| --- | --- | --- |
| 02 Geometry Storage & Streaming | Existing geometry/cluster save, load, upload, summaries, file size and timing comparisons; fully resident data | Store/upload module 01's coarse/fine geometry, hierarchy, errors and bounds |
| 07 GPU Tessellation & Displacement | Fixed-level GPU subdivision and height-map displacement on a triangle or fixed clusters; controls, wireframe, counts, timings and data sizes | Consume 03/04 selections; provide geometry to 05 and displacement bounds to 04 |

Demand paging/streaming, real GPU LOD selection, occlusion culling, and matching
displaced shadows are not prerequisites for this Part 2 M2 demonstration.

## 2. Implementation defaults

| Decision | Default |
| --- | --- |
| Generation path | Compute shaders write resident vertex/index buffers consumed by the graphics pipeline |
| Input fixtures | One triangle, a plane with continuous UVs, then a small mesh with fixed clusters |
| Subdivision setting | Uniform edge segment count `n` in `{1, 2, 4, 8}`; `n` is not a recursive subdivision depth |
| Geometry ownership | Source geometry stays separate and unchanged; module 07 owns generated resources |
| Persistence | ACR v1 stores source geometry and its existing partition; generated geometry is a runtime product |
| Initial selection | CPU-authored fixed cluster IDs uploaded as input; actual new geometry is generated on the GPU |
| Initial scheduling | One compute-capable graphics queue and the existing single-frame resource lifetime model; no async-compute requirement |
| Display | A small rendering adapter consumes the generated view; retain the original-geometry comparison path |

Compute is chosen to make the 07-to-05 handoff explicit. A switch to hardware
tessellation would change this output contract and should be coordinated before
either module implements the adapter.

## 3. Source data contract

The existing [M1 interface](milestone-1-interface.md) remains authoritative for
source data. In particular:

- `SceneData` owns original vertices/indices, topological position IDs, reordered
  render indices, cluster records, and triangle capacity.
- A source cluster ID is its array index in that specific partition. It survives
  ACR roundtrips but is not a persistent identity across rebuilding/reclustering.
- `SceneGpuData` is a borrowed view. `VulkanRenderer` owns the source buffers.
- Vertex layout is 32 bytes: position at 0, normal at 12, UV at 24. Indices are
  uint32. GPU cluster records have a 48-byte stride.
- Vertex and index views currently share an allocation. Buffer offsets are bytes;
  a cluster's `firstIndex` and `indexCount` are index-element units.
- Coordinates, displacement amplitude and bounds are in model space. The renderer
  applies the same camera/transform convention to source and generated geometry.

The source view now has a runtime `sceneRevision`. The source resource owner
increments it after every successful source upload/replacement,
including a new partition of the same model. Selections and generated results must
identify this revision. Reusing the same counts or Vulkan handles does not prove
that the scene is unchanged. A failed replacement leaves the current revision valid.
This is runtime identity metadata, not a new ACR v1 field.

## 4. Module 07 input

The following describe the intended generation inputs, not a frozen GPU ABI.
`ClusterSelection`/`ResolvedClusterSelection` implement the CPU selection semantics
below; Steps 4/5 bind an expanded source triangle work list and a uniform edge
segment count. Step 6 adds a sampled height image and displacement settings.

| Input | Required meaning |
| --- | --- |
| Source view | Borrowed `SceneGpuData` plus `sceneRevision` |
| Selection | Explicit `All` or `Explicit` mode; source revision, cluster IDs and valid count |
| Settings | Enabled flag, edge segment count, finite nonnegative displacement amplitude |
| Height field | Sampled 2D image view/sampler, dimensions, numeric encoding and revision |
| Capacity policy | Maximum permitted output bytes/counts, checked before generation |

Selection rules:

1. `All` means source IDs `0..clusterCount-1` in ascending order.
2. `Explicit` preserves the supplied order. IDs must be unique and in range;
   reject invalid or duplicate IDs rather than silently changing the request.
3. An explicit empty list means zero output, not `All`.
4. The selection must refer to the currently uploaded partition/revision.
5. M2 may validate and expand fixed work lists on the CPU. M3 GPU-produced lists
   still require a separate agreement on GPU counts, work expansion, dispatch and
   synchronization; changing the list's producer alone does not complete integration.

For uniform triangle subdivision, `T` selected source triangles produce `T*n*n`
output triangles. Calculate capacity using checked 64-bit arithmetic, then verify
that index/count limits, storage-buffer limits and the memory budget permit the
request. Reject an excessive request before writing or allocating its output.
Vertex sharing is an implementation choice; `n=1` need not preserve source vertex
count or byte ordering, but zero-displacement triangle positions and winding must match.

Height/displacement convention:

- Height samples are numeric linear values in `[0,1]`, not sRGB colors.
- The first demo uses a documented UV orientation, clamp-to-edge sampling and
  explicit base-level sampling so CPU reference checks can use the same convention.
- Use `d = amplitude * (2*h - 1)` along a normalized direction. Constant `h=0.5`
  and amplitude zero are neutral cases. Missing/disabled displacement is neutral.
- Interpolate source position/UV/normal consistently. Test fixtures have valid UVs
  and normals. Specify and test invalid/degenerate input handling before claiming
  support for arbitrary imported meshes.
- The existing four-tetrahedron mock has zero UVs; keep it for storage checks and
  use a new continuous-UV plane for displacement checks.
- Displaced shading must have a defined normal policy. The exact smooth-normal
  and seam policy remains an implementation detail to check with module 05;
  zero-displacement comparison must preserve the intended source shading.

## 5. Module 07 output and module 05 consumption

Module 07 owns an output allocation and exposes a borrowed draw view:

| Output field | Meaning |
| --- | --- |
| Source/output identity | Source scene revision and generated-result revision |
| Vertex view | Buffer, byte offset, valid vertex count, stride and attribute layout |
| Index view | Buffer, byte offset, valid uint32 index count |
| Draw ranges | `sourceClusterId`, `firstIndex`, `indexCount` for each selected source cluster |
| Counters | Selected source triangles, generated vertices/triangles, valid bytes and allocated bytes |
| Displacement bound | Conservative maximum displacement in the source model space |
| State | Whether a result matches the current source, selection and settings |

The initial output uses the existing 32-byte vertex layout and global uint32
indices relative to the output vertex view (`vertexOffset` in indexed draw calls
is zero). Preserve source winding. Draw ranges partition the valid output index
list and preserve selection order. Their index counts may exceed the M1 source
cluster capacity: a source cluster with 64 triangles can generate many more.

Do not overwrite `SceneData`, rewrite the source partition, or run generated
geometry through M1's source-triangle-preservation validator. Generated geometry
needs its own checks. Mapping back to the source cluster is retained for debugging
and future material/selection integration.

An empty selection results in zero generated draw calls. Disabling module 07
routes the same selected source geometry to the original path; it must not turn
an empty or partial selection into Draw All. The standalone demo defaults to `All`.

## 6. Ownership, synchronization and invalidation

| Resource | Owner | Borrower/lifetime |
| --- | --- | --- |
| Source buffers | Existing renderer/source uploader | 07 and 05 borrow until a successful source replacement or destruction |
| Height images, sampler and generation descriptors | 07 resource owner | Remain alive for every in-flight generation that references them |
| Generated geometry and its range metadata | 07 resource owner | 05 borrows; it must neither free nor retain obsolete output handles |
| Command buffer/submission and frame completion | Renderer integration point | 07 records generation and required dependencies in the agreed frame schedule |

Reuse the renderer's completion fence before overwriting, resizing or freeing
resources still used by a previous frame. Record generation after the relevant
uploads and before its draw. Make transfer writes visible to compute reads, and
compute writes visible to vertex/index reads. Retain generated data across frames
when valid. Future indirect-command consumers will need their own dependencies.

The current source descriptor set exposes only cluster data to the vertex stage;
it is not already a compute interface. Add a compute-specific layout/bindings
without silently changing the bounds pipeline's contract. Read packed vertex
fields with an explicitly matching layout; do not assume two std430 `vec3`s match
the CPU struct. Respect the combined source buffer's byte offsets and storage
descriptor alignment requirements when binding index data.

Device selection now requires a graphics queue with compute support and the
generation pipeline's workgroup/descriptor limits (Steps 4/5). Wireframe checks and
enables `fillModeNonSolid` only when supported. Upload timestamps check support too.

The cache key includes scene revision, selection contents/order, subdivision
setting, amplitude, height-field revision and sampling policy. A change invalidates
the result. Camera motion alone does not invalidate fixed-input M2 geometry.
New scene data, even with identical counts, invalidates the old result. Do not label
an old result as matching newly requested settings if regeneration failed.

## 7. Module 04 displacement handoff

With normalized displacement directions, the height convention above guarantees
`abs(d) <= amplitude`. For the initial linear subdivision, expanding each source
AABB by `amplitude` on every model-space axis is conservative. Pass this bound to
04 before it culls the input; do not require geometry readback to discover it.
Any later change to the displacement function or coordinate transforms must also
update this bound. Formal culling integration remains M3 work.

## 8. Storage and measurement handoff

- Keep ACR v1 and its original/reordered indices and position IDs intact.
- Do not serialize device handles, generated runtime output or runtime revisions
  into the existing format. New hierarchy fields require a versioned agreement
  with module 01 before formal persistence/upload integration.
- Module 02 supplies file bytes, scene counts, CPU load/validation duration and
  clearly labelled upload measurements. Module 07 supplies generation counts,
  valid/allocated data bytes and generation timing. Module 06 owns the eventual
  common measurement/presentation integration.
- Separate CPU load/end-to-end wait time from GPU transfer/compute time. Use GPU
  timestamp queries where supported; report unavailable measurements explicitly.
- Label reused geometry and the last generation time. A benchmark must explicitly
  regenerate fixed input, warm up, and report repeated measurements. Buffer
  readback for validation is not part of those performance measurements.

## 9. Shared integration points for team review

| Counterpart | Concrete agreement to confirm |
| --- | --- |
| Member 1, 01 | Future hierarchy/coarse-geometry schema, IDs, coordinate space and geometric-error definition |
| Member 1, 03/04 | Scene/selection revision, unique cluster ID list, valid count, GPU producer synchronization and displacement bounds |
| Member 3, 05 | Acceptance of compute-generated buffers, vertex/index layout, ranges and original/generated draw routing |
| Member 3, 05/06 | Named owner for shared Renderer/Viewer changes, wireframe support and common profiling hooks |

Keep 07 implementation and its independent fixtures separate from the shared
integration adapter. Do not assume another member has accepted these defaults
until that review occurs. Independent fixed-input work can proceed from this contract.

## 10. Step 1 verification record

Verified locally on 2026-11-13 with Visual Studio 2022, the Vulkan SDK and the
NVIDIA GeForce RTX 4070 Laptop GPU:

```powershell
cmake --preset windows-vs2022 -DACR_ENABLE_GPU_TESTS=ON -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build --preset debug --parallel 1 -- /nodeReuse:false
ctest --test-dir build -C Debug --output-on-failure
```

The disconnected-update setting reused already downloaded pinned dependencies.
Debug configuration/build succeeded. The final CTest run passed **10/10 tests**:
six CPU scene tests, one CPU preprocessing test and three GPU smoke tests. The
GPU logs reported validation enabled and passed the readback/draw/scene-replacement
checks. This step did not repeat the earlier interactive UI review or measure
Release performance.

The initial upstream test run passed 9/10: `scene-qem-guided` incorrectly required
the QEM and baseline builders to produce identical cluster counts for the same
capacity. On bundled Suzanne, baseline grouping produces 117 clusters and QEM
grouping produces 145, both retaining 3,936 triangles at capacity 64. Different
growth orders can leave different remainder groups, so equality between the two
counts is not a valid invariant.

The local test correction checks the legal count interval and retains deterministic
output, complete triangle coverage, winding, capacity and bounds validation. It
also checks ACR roundtripping of the QEM partition and finite, nonnegative error
statistics. Neither clustering algorithm was changed. Passing this test makes no
claim that one partition is faster or has better visual quality.

The foundational deliverables are the synchronized source baseline, this contract,
the focused test correction and the README handoff link. Implementation of the
M2 runtime features starts in subsequent steps. Team acknowledgements listed in
section 9 remain coordination items, not completed reviews.

## 11. Step 2 implementation and verification

Module 02 now exposes runtime-only diagnostics in `SceneDiagnostics.h`:
`LoadedScene` combines the existing `SceneData` with its source kind/path,
optional disk bytes/version and CPU preparation time. `SceneUploadStats` reports
the last successful upload's valid payload, resident allocation bytes, CPU upload
wall time and an optional GPU timestamp interval. Existing `loadScene`/`saveScene`
and ACR v1 records remain unchanged; these diagnostics do not enter the file.

The Viewer and CPU preprocessing/validation commands use the same summaries.
OBJ preparation includes cluster construction; ACR loading reads the existing
partition. Generated Mock/Cube sources have no file size or file version.
Resident allocation bytes cover only the device-local mesh and cluster buffers.
Original CPU indices and position IDs remain on disk/CPU and are not counted in
the rendering payload. Staging, framebuffer and UI resources are excluded.

GPU transfer timing uses two timestamp queries on the selected graphics queue,
reset in the upload command buffer, placed around its two copy commands. Query
results are read after queue completion, converted using `timestampPeriod` and
masked to `timestampValidBits`. Unsupported queues report N/A. The CPU upload
timer also includes validation/packing, allocation, staging, waiting and publishing
the descriptor. Debug readback runs separately. All values are diagnostic samples;
no comparative performance claim or speed threshold follows from them.

`saveSceneVerified` writes a cache and reloads it, then compares every persisted
field, including float bits and both index orders. An `.acr` extension is required
before opening the output. **Reload saved ACR** checks the file still matches,
then preserves the current camera and cluster selection while uploading it again.
Normal **Open** and **Load mock** fit a new camera and replace source diagnostics.
Invalid input leaves the current valid scene/measurements available. A save alone
adds a separate last-save record and does not claim the displayed source changed.

Local Debug verification on 2026-11-13 passed **11/11 CTest cases**: seven CPU scene
cases, one CPU preprocessing case and three GPU smoke cases on the RTX 4070 Laptop
GPU with Vulkan validation enabled. New coverage includes exact disk/payload sizes,
source metadata, field-sensitive equality (including signed zero), finite timings,
load rejection, wrong-extension output preservation, save/mock/disk-reload equality,
GPU readback, allocation bounds and unchanged diagnostics after a rejected upload.

Interactive Windows verification covered saving Suzanne, exact-match confirmation,
reloading with the current zoom and selected cluster preserved, expanding payload
details, scrolling the panel, switching to Mock, rejecting an invalid ACR while
keeping Mock, and reopening the valid Suzanne cache. The interactive test file
is under ignored `build/test-data/gpu/ui-step2-verified.acr`. The unsupported-GPU-
timestamp branch is capability-guarded but was not exercised on this GPU.

This completes the storage-summary step. Module 07 fixtures, subdivision,
displacement and compute-to-render integration are subsequent steps. The common
measurement handoff to module 06 and hierarchy schema agreement remain open.

## 12. Step 3 fixtures and selection

`TessellationFixtures.h` adds deterministic CPU-authored source scenes, independent
of either cluster builder. `SceneDiagnostics` wraps them as generated sources;
file size/version remain N/A until an ACR is actually loaded. The viewer and
`--fixture triangle|plane|grid` share these constructors.

| Source | Vertices | Triangles | Clusters | Capacity | ACR v1 bytes | GPU payload bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Triangle | 3 | 1 | 1 | 1 | 244 | 156 |
| Plane | 4 | 2 | 1 | 2 | 304 | 200 |
| Grid (2 x 2 patches) | 9 | 8 | 4 | 2 | 772 | 576 |

Coordinate reference:

- Geometry lies in model-space XY, `z=0`, bounded by `[-1,1]` in X/Y.
- Triangle corners are `(-1,-1,0)`, `(1,-1,0)`, `(-1,1,0)`, in that order.
- Normals are unit `(0,0,1)`; triangle cross products point along +Z.
- UV is `((x+1)/2, (y+1)/2)` across the entire surface. U increases toward +X;
  V increases toward +Y. The future height sampler must use this numeric
  convention explicitly, including any image-loader row orientation.
- The plane has one shared diagonal. The grid has eight shared edges, four of
  which cross cluster boundaries, plus eight outer boundary edges. Adjacent
  triangles share vertex IDs, UVs and normals; edges use opposite winding.
- Grid clusters run left-to-right, bottom-to-top: bottom IDs 0/1, top IDs 2/3.
  Cluster `i` uses `firstIndex=6*i`, `indexCount=6`; its AABB covers that unit patch.
  IDs and ranges are authored and survive ACR roundtrips without reclustering.

Subdivision reference introduced in Step 3 (now generated and verified in Step 5):

| Selection | Input triangles | n=1 | n=2 | n=4 | n=8 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Triangle: All | 1 | 1 | 4 | 16 | 64 |
| Plane: All | 2 | 2 | 8 | 32 | 128 |
| Grid: All | 8 | 8 | 32 | 128 | 512 |
| Grid: IDs 0,3 | 4 | 4 | 16 | 64 | 256 |
| Explicit empty | 0 | 0 | 0 | 0 | 0 |

`ClusterSelection` carries source revision, All/Explicit mode and an ordered ID
list. `resolveClusterSelection` consumes validated source cluster metadata and
returns original source IDs/ranges and a 64-bit selected index count. It never
rebuilds, mutates or renumbers source data. All requires no ID list and resolves
in ascending order. Explicit preserves order, including a full reversed list;
empty Explicit produces zero ranges. Duplicate/out-of-range IDs, unknown modes,
ambiguous All-plus-ID requests and stale/zero source revisions are rejected.

`SceneGpuData.sceneRevision` starts at zero, increments on every successful
upload, and stays unchanged on failure. The renderer defaults a new source to
All; `setClusterSelection` publishes a newly resolved selection only on success.
The source preview routes both shaded/colored mesh draws and bounds through that
selection, preserving original cluster colors and bounds IDs. Empty input records
zero mesh draws and zero bound instances. These are CPU-authored source draw lists;
no GPU list buffer, compute pipeline or generated geometry is claimed in this step.

The UI exposes All / First + last / Empty presets. A single-cluster source is
included once by First + last. Regular source switching resets to All. Exact-match
saved reloads rebind the prior selection to the new revision and retain the camera.
Source files/payloads always describe the whole source, even for partial preview.
ACR persists the complete source only; revisions and selections are runtime data.

Verification on 2026-11-13: Debug build and **18/18 CTest cases** passed (11 CPU
scene/fixture cases, one CPU preprocessing case, six GPU smoke cases). CPU checks
cover the table above, deterministic creation, geometry/UV/normal/winding/area,
edge adjacency, exact roundtrip and selection rules. GPU tests include all three
new fixtures, readback, actual disk reload, all/partial/empty recorded draw counts,
bounds filtering, rejection preservation and revision invalidation, with Vulkan
validation enabled on the RTX 4070 Laptop GPU. Interactive checks confirmed all
three scenes, grid colors/bounds, 0/3 preview (two patches/four triangles), empty
preview (no mesh/bounds), and reset-to-All when switching from empty to a new scene.

Step 4 below adds compute-capable queue selection, input/output GPU bindings,
copy-through, synchronization and a generated-geometry draw adapter.

## 13. Step 4 compute copy-through

The original `GeometryCopyPlan` (generalized to `GeometryGenerationPlan` in Step 5)
checks capacity and expands the ordered selected ranges into
source triangle first-index addresses on the CPU. It does not generate vertex or
index geometry. `ComputeGeometry` owns the compute pipeline, five storage-buffer
bindings, work-list upload, independent output buffers, timestamps and cache key.
`VulkanRenderer` owns submission and adapts the borrowed generated view for draws.

One invocation handles one source triangle; a workgroup has 64 invocations and
the last group guards excess invocations. The shader reads three source indices
and copies eight uint words per source vertex, emitting three unshared vertices
and sequential global uint32 indices. The raw-word layout matches position at
byte 0, normal at 12, UV at 24 and a 32-byte stride, without relying on std430
vec3 packing or scalar-layout features. Copy-through preserves all attribute bits
and winding, with `maxDisplacement=0`. Original source data/ACR v1 are unchanged.

| Binding | Data / access |
| --- | --- |
| 0 | Source vertex words, read only |
| 1 | Source index words, read only |
| 2 | CPU-authored triangle first-index addresses, read only |
| 3 | Generated vertex words, write only |
| 4 | Generated indices, write only |

Source descriptors start at byte zero; a 12-byte push-constant block supplies
vertex-word offset, index-word offset and triangle count. This handles a combined
source VB/IB whose logical index offset is not storage-descriptor aligned. Ranges
include the logical offsets and are checked against `maxStorageBufferRange`.
Source data is assumed validated by the uploader. The existing bounds descriptor
set remains separate and unchanged.

`GeneratedGeometry` exposes borrowed vertex/index handles, byte offsets, strides,
valid counts, compact output draw ranges retaining original source IDs, source and
generation revisions, valid/payload/allocation counters, zero displacement bound,
CPU generation time and optional GPU dispatch time. `valid` must be checked after
any source/selection change. Output ranges are contiguous even for a partial or
reversed source selection; non-colored output uses one indexed draw. Colors and
bounds still refer to original cluster IDs. Zero displacement permits using the
source bounds; this assumption must change when displacement is added.

| Input | Generated vertices | Generated triangles | Valid output bytes |
| --- | ---: | ---: | ---: |
| Triangle, All | 3 | 1 | 108 |
| Plane, All | 6 | 2 | 216 |
| Grid, All | 24 | 8 | 864 |
| Grid, 0,3 or 3,0 | 12 | 4 | 432 |
| Explicit empty | 0 | 0 | 0 |
| Suzanne, All | 11,808 | 3,936 | 425,088 |

Lifecycle and limits:

- The renderer waits for its frame fence before replacing output resources.
  Source transfer writes become visible to compute reads; work-list host writes
  become visible to compute reads; compute writes become visible to vertex/index
  and debug-transfer reads. Readback copies have a transfer-to-host dependency.
- Generation submits on the same graphics/compute queue and waits for completion
  before publishing a valid result. It runs on changed input or explicit regenerate,
  not every frame. This synchronous M2 path does not claim async overlap.
- Cache identity includes source revision, source buffer handles/offsets/counts/
  strides, selection mode and ordered IDs/ranges. Equivalent or rejected selection
  requests preserve the cache. Successful source replacement invalidates it even
  with identical geometry/counts. Camera motion and source/output display toggles
  reuse valid output. Step 5 adds subdivision level; future height settings must extend this key.
- Empty input frees old outputs, produces a valid zero-count result, and records
  no dispatch or mesh/bounds draw. No zero-sized buffers/descriptors are created.
- Checked 64-bit counts precede work-list expansion and output allocation. The
  copy's output budget is 128 MiB, including actual allocation alignment; also
  check uint32 counts, storage ranges and X workgroup count. Work-list/source
  allocations are separate. Replacement may temporarily retain both old and new
  outputs. Allocation/limit failure leaves the requested output invalid and the
  interactive viewer reports the failure while drawing source geometry.
- CPU generation timing includes planning, allocation, work-list upload, submission
  and queue wait; it excludes the preceding frame fence wait and debug readback.
  GPU timing covers the dispatch interval, not subsequent drawing. Empty output
  and unsupported timestamps report N/A. Last-generation timings are diagnostic
  samples, not performance benchmarks.

Verification on 2026-11-13: Debug build and **26/26 CTest cases** passed (13 CPU
scene/fixture/copy-plan cases, one preprocessing case and 12 GPU smoke cases).
The GPU run used `VK_LAYER_VALIDATE_SYNC=1` with the Khronos validation layer on
the RTX 4070 Laptop GPU and produced no validation warnings/errors. Copy tests
read back each output vertex/index and compare attributes bit-for-bit against an
independent walk over the selected source ranges, then read back the source to
confirm it was not changed. Coverage includes guarded partial workgroups, source
IDs/order, counts, empty output, source/output routing, rejection preservation,
cache hits, forced regeneration and same-source revision replacement. CPU checks
cover budget/storage/dispatch boundaries and uint64-to-uint32 overflow rejection
without large allocations. Unsupported timestamps and actual device out-of-memory
failures were not exercised on this machine.

Interactive checks confirmed identical Suzanne appearance, camera rotation with
the same generation number, explicit regeneration, scene switching, grid IDs 0,3
and 3,0 with unchanged colors, and empty output with no residual mesh/bounds.
This stage established the compute-to-render path. Step 5 below adds uniform
subdivision and wireframe; height displacement remains Step 6.

## 14. Step 5 uniform GPU subdivision

The generalized `GeometryGenerationPlan` and `generate_geometry.comp` replace the
copy-only names from Step 4. The same five storage bindings and 32-byte vertex ABI
remain in use. The compute push constant is now 16 bytes: source vertex word
offset, source index word offset, source triangle count, and edge segment count n.
The accepted levels are exactly 1, 2, 4 and 8. Invalid levels are rejected without
changing the current setting/cache. Output counts use checked 64-bit products;
the source address work list still has one uint32 per selected source triangle.

For T selected input triangles:

- Generated triangles: `T*n*n`; vertices and indices: `3*T*n*n` each.
- Valid output bytes: `108*T*n*n`; work-list bytes: `4*T`.
- Dispatch X count: `ceil(T*n*n/64)` with one invocation per generated triangle.
- Draw ranges scale each source cluster's index count by n squared while retaining
  its source ID/order. The output allocation, storage range, count and dispatch
  checks all happen before expansion/allocation. The 128 MiB output budget remains.

Each GPU invocation identifies its source triangle and decodes a small triangle
on a barycentric lattice. Row j has `2*(n-j)-1` triangles. Both lattice orientations
retain the original winding. Positions, UVs and normals are linearly interpolated;
original corners are copied as raw words (n=1 remains a bit-exact copy). Normals
are deliberately not normalized at new vertices: the existing fragment shader
normalizes after interpolation, preserving the source's lighting field. Step 6
must normalize displacement directions separately and define displaced shading.

On a shared indexed edge, endpoint indices define a canonical operand order and
the shader uses `precise` interpolation. Both traversal directions therefore emit
bit-identical position/UV/normal samples for the same indexed edge. All selected
triangles use the same n, including across cluster boundaries. This does not solve
different per-cluster levels or displacement across split UV/normal seams. The
fixtures have continuous shared indices, UVs and normals as documented in Step 3.

`GeneratedGeometry` adds `edgeSegments` and `sourceTriangleCount`; n joins the cache
identity. A valid source replacement, changed selection or changed n invalidates
output. Camera motion, wireframe toggles and source/generated comparison reuse
the valid output. Empty selections remain empty at every n, including when n
changes. Saving ACR still persists the full unchanged source.

The viewer exposes **GPU subdivision**, **Segments per edge (n)**, **Wireframe**,
generation number and counts/timings/allocations. `--subdivision 1|2|4|8` enables
generation at the requested level; `--compute-copy` is retained for the n=1 baseline.
`--wireframe` selects a line-mode graphics pipeline. The device feature is queried
and enabled explicitly; unsupported hardware disables the UI control and rejects
an explicit CLI wireframe request. Filled mode remains available. The wireframe
pipeline uses the same geometry/layout and is recreated and destroyed with the
other graphics pipelines. It is not a filled overlay or an anti-aliased line mode.

`GeometryValidation` is a debug CPU oracle, not a geometry-generation path. It
enumerates lattice cells independently of the shader's linear invocation decoder,
uses a double-precision barycentric reference with scale-aware tolerance for each
attribute, checks exact original corners and sequential output indices, compares
shared indexed-edge samples bitwise, and checks orientation and total source area.
Readback/source-immutability checks are excluded from generation timing. The
verifier itself is tested with a hand-authored n=2 triangle and intentional wrong
UVs, normals, positions, winding, counts, indices and a sub-tolerance edge mismatch.

Verification on 2026-11-13: Debug build and **34/34 CTest cases** passed: 15 CPU
scene/fixture/plan/reference cases, one preprocessing case and 18 GPU smoke cases.
Khronos validation and `VK_LAYER_VALIDATE_SYNC=1` were enabled on the RTX 4070
Laptop GPU; there were zero validation warnings/errors. Every new GPU smoke case
exercises all levels, including reset to n=1, partial/reversed/empty selections,
level changes while empty, equivalent/rejected setting requests, source replacement,
wireframe and source/output switching. Suzanne at n=8 produced **755,712 vertices /
251,904 triangles** and passed 218,070 shared-edge comparisons in a full readback.
The maximum attribute error against the double-precision reference across this
test run was `1.3411e-7`; the authored plane/grid attributes were exact. These are
correctness results, not speedup/FPS benchmarks. Unsupported wireframe/timestamps
and actual device out-of-memory conditions were not exercised on this GPU.

Interactive checks first reconfirmed Step 4's statistics layout, source/output
comparison, camera motion and cache reuse. Step 5 checks confirmed 1/4/16/64
triangles for a single triangle, 512 for the full grid at n=8, 256 for IDs 0,3,
empty output with no residual mesh, source/generated topology comparison, filled/
wireframe switching without regeneration, and panel scrolling at the default
960x640 window size. No UI issue remained from those checks.

Step 6 below adds height-map displacement. Shape smoothing, adaptive per-cluster
levels, general seam stitching, GPU LOD/culling and displaced shadows are outside Step 5.

## 15. Step 6 GPU height-map displacement

`HeightField` stores validated linear float heights, dimensions and a display name.
The built-in Waves, Mound, Ramp U/V, Neutral, Black and White fields are 33x33.
Waves is loaded by default; displacement is disabled by default, with amplitude
0.25 ready for the UI toggle. `loadHeightField` accepts a single PGM P2 or P5 image,
maximum value 1..65535, binary 16-bit samples in big-endian order. Dimensions are
limited to 1..2048 per axis before allocation (at most 16 MiB of float payload).
Missing/truncated/excess/out-of-range samples and invalid headers are rejected.
PGM comments are supported in header/ASCII tokens; binary raster bytes are never
treated as whitespace/comments. CRLF and CR-only binary separators are resolved
using the expected raster byte count. Samples are numeric linear data, not sRGB.

Module 07 owns a device-local `R32_SFLOAT` image, memory, image view and sampler,
plus the CPU height field for reference checks and bounds. The compute descriptor
layout retains five storage bindings and adds **binding 5**, one combined image
sampler. Its push constants are now **24 bytes**: four uint32 values from Step 5,
then float amplitude and float maximum displacement. The 32-byte vertex ABI,
source IDs, triangle work list and output count/budget rules are unchanged.

Image creation checks sampled-image/transfer format support and image dimensions.
Uploads stage the float samples, transition UNDEFINED to TRANSFER_DST_OPTIMAL,
copy the buffer into the image, then transition to SHADER_READ_ONLY_OPTIMAL with
transfer-write to compute-read visibility. The renderer waits for the previous
frame before replacement; upload uses the same graphics/compute command buffer
and queue and waits before updating the descriptor/publishing the new revision.
Old resources remain valid until the replacement succeeds. Invalid fields/settings
leave the active image, output cache and revisions intact. CPU field copies and
temporary old/new images plus staging are additional peak memory, separate from
the output budget. The height allocation is shown separately in the viewer.

Sampling is fixed for M2: row 0 corresponds to v=0, without automatic vertical
flip; UVs clamp to [0,1]; UV endpoints address the first/last texel centers.
The shader uses four `texelFetch` calls and explicit bilinear interpolation at
mip zero. A nearest sampler avoids requiring optional linear filtering support
for R32_SFLOAT. One-pixel/one-axis images have well-defined clamped coordinates.
Changing the sampling policy in a future version must also change cache identity.

After linear subdivision the GPU applies
`p' = p + normalize(interpolated source normal) * amplitude * (2*h - 1)`.
Normalization first scales by the maximum absolute component to avoid squaring
large source normals. A component maximum <=1e-20 uses a zero displacement
direction. Finite amplitudes in [0,1000] model units are accepted; the demo slider
is [0,1]. Large offsets may fold/intersect an imported surface; no collision or
self-intersection correction is implied.

With a positive global displacement bound, each generated triangle receives its
own geometric face normal from the displaced positions. This is deliberately
**flat shading**, even where a nonneutral image happens to sample neutral values.
For a degenerate face, use the normalized interpolated source normal, then +Z if
that is also zero. When displacement is off, amplitude is zero, or the entire
image is exactly neutral, the shader bypasses both displacement and normal
replacement: Step 5's original-corner bits and interpolated shading are preserved.
No source vertices or cluster records are changed.

Canonical shared-index edge interpolation is retained before sampling. Matching
shared indexed-edge position/UV/normal inputs therefore produce identical final
positions and UVs, including across cluster IDs at the same n. Geometric face
normals may differ across that edge. Split UVs or normals on duplicated vertices
are not welded; arbitrary imported seams and different per-cluster n are still
outside this implementation. The continuous-UV plane/grid are the primary demo.
Mock's zero UVs are useful for storage and constant-image cases, not a wave demo.

The conservative scalar bound is
`B = enabled ? amplitude * max(abs(2*hMin-1), abs(2*hMax-1)) : 0`.
Bilinear interpolation remains inside that height range. Expand each source
model-space AABB by B along all three axes; `GeneratedGeometry.maxDisplacement`
publishes B. The CPU `displacementBound(height, settings)` also makes it available
**before** generating geometry for a future module 04 consumer. No readback is
needed to obtain it. The bounds vertex shader now reads padding at push-constant
offset 80; the graphics layout is one 84-byte vertex+fragment range, and graphics
pushes use both stage flags. Source rendering supplies padding zero. Actual culling
integration, transform-aware bounds and matching shadows remain later team work.

The output records displacement settings and height revision. Scene/selection/n,
enabled/amplitude, or a new height revision invalidate it; reloading an identical
height image still advances the revision. Repeating equivalent settings, camera
changes and source/wireframe comparison reuse output. An explicit empty selection
remains zero output even through image/settings changes. Image uploads are separate
from the reported generation timings; dispatch timing includes height sampling
and normal computation. Saving ACR continues to save the full unchanged source;
height data/settings and generated output are not serialized into ACR v1.

Verification on 2026-11-14: Debug build and **41/41 CTest cases passed** (18 CPU
scene/fixture/plan/reference cases, one preprocessing case, 22 GPU smoke cases),
with Khronos validation and `VK_LAYER_VALIDATE_SYNC=1` on the RTX 4070 Laptop GPU.
There were no Vulkan validation warnings/errors. The three new CPU cases exercise
sampling/format/invalid-input rules and a hand-authored analytic ramp, including
nonunit/zero source normals and intentional position/UV/normal corruption. Four
new GPU cases cover triangle, plane, grid and Suzanne OBJ with source/ACR/mock
replacement, all n levels, image upload/reload, black/white/ramps/waves/mound,
neutral/off/zero, partial/reversed/empty selections, zero source normals, invalid
settings preservation, source immutability and padded bounds.

GPU readback uses the independent double-precision barycentric and bilinear
reference for positions/UVs; active displacement normals are compared with each
actual generated face. Shared-edge positions/UVs are checked bitwise. Original
area/winding checks apply to the neutral path; an actively displaced surface can
legitimately change area or fold. Across the full run, maximum attribute error was
`2.0396e-7` and maximum normal-vector error was `4.12656e-5`; Suzanne at n=8 still
produced 755,712 vertices / 251,904 triangles. These are correctness results, not
performance benchmarks. After UI refinements and extra CR/CRLF parser cases,
the three displacement CPU tests and the grid GPU test passed again.

Interactive checks confirmed visible waves/mound, amplitude changes with matching
bounds padding, neutral/off restoration, filled/wireframe and source comparison,
zoom with the same generation number, and PGM file import into a tilted plane.
The height allocation label was shortened to fit, and the file picker starts
beside the bundled example. Controls and output statistics were inspected both
maximized and by scrolling the default 960x640 window. PNG/JPEG, smooth displaced
normals, general seam repair, adaptive tessellation, streaming and shadows are not
included in this step.

## 16. Step 7: complete demo and repeatable measurement

The [demo guide](milestone-2-part2-demo.md) joins storage, fixtures, selection,
subdivision, displacement, source comparison and performance measurement into one
walkthrough. Its handoff table separates the completed independent M2 scope from
the later hierarchy/visibility/rendering/shadow integration.

`BenchmarkSession` accepts a fixed context and configurable warmup/sample counts
(defaults 10/50), discards warmup, and validates timing availability and increasing
generation revisions. Generated runs explicitly force regeneration even when the
cache key matches. Source runs have no generation metrics; empty selections have
no dispatch metric; unsupported timestamp queries remain null. Cancelled/unfinished
runs cannot be exported. Completed JSON records measured samples, summaries,
configuration, camera, selected source IDs, draw counts and payload/allocation sizes.

`VulkanRenderer` owns a separate two-query render timestamp pool. Measured frames
reset/write it, submit after a TOP_OF_PIPE image-availability wait, then read both
timestamps after the frame fence. The caller consumes a measured frame before
submitting another; a missed consumption is rejected. Valid timestamp bits and
the physical device's timestamp period are applied exactly as in upload/compute
timing. The interval includes the render pass and its clears, mesh, optional
bounds and optional UI. It excludes compute and presentation/acquisition waits.
The ordinary rendering path retains its original wait stage and has no new query
work. Unsupported devices export null GPU measurements.

During an interactive batch, scene/settings/camera are locked. A resize cancels
the batch. CLI `--benchmark output.json` runs a hidden fixed-input batch with no
ImGui drawing. `--benchmark-warmup`, `--benchmark-samples` and `--selection` make
cases reproducible; smoke and benchmark modes cannot be combined. Correctness
readbacks run outside sampling when requested. On completion, the generated cache
must be reusable again. A successful run exports only after all requested samples
are collected; a hidden run exits with an error if interrupted or incomplete.

CPU generate-plus-wait still includes per-run reallocation, worklist preparation,
submission and queue waits. GPU generate is the dispatch interval. Render timing
is a pass interval rather than frame time/FPS. Startup load/upload counters remain
single-operation diagnostics and are explicitly labelled `startup_` in reports.
The [Release snapshot](milestone-2-part2-performance.md) records seven sequential
cases and their limits; there is no CPU/GPU speedup claim or controlled-clock claim.

Verification on 2026-11-14: 48/48 Debug CTest cases passed (21 CPU cases, one
preprocessing case, 26 GPU cases) with synchronization validation, no Vulkan
validation warnings/errors; seven benchmark-focused cases also passed in Release.
The new integration cases parse persisted JSON and cover all/reversed/empty/source
paths, increasing generation revisions, metric availability, fixed draw counts and
pre/post-batch geometry readbacks. Interactive testing covered successful export,
settings lock, cancellation, resize invalidation and restored controls. A long
result heading was changed to wrap after inspecting the default and maximized UI.

## References

- [M1 data and GPU interface](milestone-1-interface.md)
- [Vulkan compute shader tutorial](https://docs.vulkan.org/tutorial/latest/11_Compute_Shader.html)
- [Shader memory layout](https://docs.vulkan.org/guide/latest/shader_memory_layout.html)
- [Synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
- [Wireframe device feature](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html)
- [GPU timestamp queries](https://docs.vulkan.org/samples/latest/samples/api/timestamp_queries/README.html)
- [Timestamp execution stage](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html)
- [Submit semaphore wait scope](https://docs.vulkan.org/refpages/latest/refpages/source/VkSubmitInfo.html)
