# Milestone 1 handoff

This completes the data/visualization handoff in the team's parallel development roadmap. It is an M1 static, fully resident baseline; it makes no claim of Nanite-style LOD selection or acceleration.

## CPU data and responsibilities

`SceneData` owns a `MeshSource`, `ClusterDataset` and triangle capacity. Track A can call `buildScene(mesh, capacity)`; Track B can call `saveScene` / `loadScene`; Track C takes the same object through `VulkanRenderer::uploadScene`. No renderer path rebuilds clusters from an ACR file.

- `mesh.vertices`: model-space position, normal and UV. CPU `Vertex` is 32 bytes, offsets 0 / 12 / 24. UVs are retained even though the M1 shader does not texture the model.
- `mesh.indices`: original triangle order, retained for later preprocessing and coverage checks.
- `mesh.positionIds`: original topological position ID for every render vertex, preserving adjacency across normal/UV seams. IDs need not be dense or equal render-vertex indices.
- `dataset.indices`: reordered triangle list, used for rendering.
- `dataset.clusters`: first index, index count, model-space AABB. **Stable ID equals array position** and survives save/load. It is stable for this partition, not a promise of identity after future reclustering.
- `maxTrianglesPerCluster`: persisted capacity; default real OBJ capacity is 64.

`validateScene` checks finite attributes, counts, indices, triangle-aligned contiguous cluster ranges, capacity, complete coverage, valid bounds enclosing the referenced geometry, and a triangle multiset comparison that preserves winding. It does not require manifold meshes or remove degenerate triangles; the original OBJ import policy is retained. Cleanup can be added before future simplification.

`makeMockScene()` authors four separate tetrahedra directly: 16 vertices, 16 triangles, four clusters with 12 indices each, IDs 0–3. It does not invoke the cluster builder, so B/C remain independently testable. Both mock and real data use the same validation, persistence, upload and draw interfaces.

## ACR v1 file

All scalar fields are explicitly encoded **little-endian**; floats are IEEE-754 binary32. C++ structs and GLM padding are never dumped to disk. The M1 loader limits files to 512 MiB and checks exact expected length before allocating payload arrays. Unsupported versions/layouts/flags, truncation, trailing data, inconsistent IDs and invalid geometry are rejected.

The header is 64 bytes:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | Magic: `ACRM1\r\n\0` |
| 8 | 4 | Version = 1 |
| 12 | 4 | Header size = 64 |
| 16 | 4 | Vertex count |
| 20 | 4 | Original index count |
| 24 | 4 | Reordered index count (must equal original count) |
| 28 | 4 | Cluster count |
| 32 | 4 | Maximum triangles per cluster |
| 36 | 4 | File vertex stride = 32 |
| 40 | 4 | File cluster stride = 48 |
| 44 | 4 | Flags = 0 |
| 48 | 4 | Position ID count = vertex count |
| 52 | 12 | Reserved, zero |

Payload order:

1. Vertices: eight floats each (position xyz, normal xyz, UV xy).
2. Original indices: uint32 array.
3. Reordered indices: uint32 array.
4. Position IDs: uint32 array.
5. Cluster records: bounds min xyz / zero padding, bounds max xyz / zero padding, firstIndex / indexCount / stable ID / reserved zero. Each record is 48 bytes.

Triangle order, normals, UVs, seam IDs and cluster order are preserved. No hierarchy, materials, source filename or platform-dependent pointer is stored. Future incompatible records require a new version and explicit loader changes.

## GPU handoff

`sceneGpuData()` returns a **non-owning** `SceneGpuData` view. The renderer owns every allocation. Views remain valid until the next successful upload or renderer destruction. Do not free them or retain handles across replacement. Upload waits for the last frame; replacement resources are prepared before the old scene is discarded.

| View | Layout / use |
| --- | --- |
| Vertex | 32-byte stride; position at 0, normal at 12, UV at 24; model space |
| Index | uint32, reordered triangle list; respect `indexOffset` |
| Cluster | 48-byte `GpuClusterDesc`, 16-byte alignment, explicit static assertions |

Vertex/index views currently share one device-local buffer. A second device-local buffer stores cluster records. Both are fully resident and support transfer readback; cluster records are uploaded through staging with a transfer-write -> vertex-shader-read barrier. The vertex/index buffer is also storage-capable for future descriptor bindings; **the current descriptor set only binds the cluster buffer**. Future storage access to the 32-byte packed vertex format must use matching scalar fields, not an assumed std430 pair of padded vec3s.

Current set **0**, binding **0** is one readonly storage buffer, visible to the vertex stage:

```glsl
struct ClusterDesc {
    vec4 boundsMin;   // offset 0, w = 0
    vec4 boundsMax;   // offset 16, w = 0
    uvec4 indexRange; // offset 32: firstIndex, indexCount, ID, reserved
};
layout(std430, set = 0, binding = 0) readonly buffer ClusterBuffer {
    ClusterDesc clusters[];
};
```

`bounds.vert` reads that buffer and generates 12 line segments per instance. All boxes use one instanced draw; a selected box uses its stable ID as `firstInstance`. Bounds share the mesh camera matrix, test against scene depth and do not write depth. This makes the data handoff visible in the demo without claiming GPU culling. Ordinary mesh rendering retains a one-draw Draw All fallback; cluster color mode still records one indexed draw per cluster on the CPU.

No hierarchy buffer is fabricated for M1. Track B/C should agree on new hierarchy records, descriptor bindings, compute-stage visibility and synchronization when adding traversal in M2.

## Verification and demo

Build and test commands are in the README. CPU tests validate real and mock scenes, exact binary roundtrips, rejected corrupt files and semantic errors. `--verify-gpu` compares the resident vertex/index/cluster bytes with the CPU scene. Optional CTest GPU runs cover OBJ and cached inputs, fixed mock, real/mock replacement, Draw All, cluster colors, and SSBO bounds. They verify buffer contents and Vulkan usage, not image correctness or performance; inspect the interactive demo for visual results.

Recommended demo: Suzanne shaded -> cluster colors -> selected bounds -> save ACR -> load the cache -> fixed mock with all four bounds -> reload a real mesh. Show the counts and explain the A/B/C handoff. The M1 acceptance basis is the team's roadmap; the instructor's submission requirements still determine the final presentation package.

### Local verification record — 2026-11-11

The Debug build passed all nine CTest cases, including three GPU smoke tests, on the NVIDIA GeForce RTX 4070 Laptop GPU. Interactive checks confirmed shaded/cluster-color switching, all/selected bounds, OBJ loading, Save ACR, Load mock, reopening the saved scene, zoom, camera reset and window maximization. The user separately confirmed that left-button orbit works; automated drag input did not reliably exercise that interaction.

The UI-saved Suzanne scene retained 2,012 vertices, 3,936 triangles and 117 clusters. Its SHA-256 matched the command-line-preprocessed scene, and GPU readback checks passed on every successful scene replacement. Loading the deliberately invalid ACR fixture displayed the expected non-finite-attribute error while preserving the previous scene. The interactive log contained no Vulkan validation warnings/errors; its one application error was the intentional invalid-file check. This is functional validation, not an FPS benchmark or a claim of synchronization-validation coverage.
