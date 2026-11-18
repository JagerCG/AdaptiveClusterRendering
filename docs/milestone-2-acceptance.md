# Milestone 2 on main

Part 2 supplies stored geometry and GPU subdivision/displacement (02 and 07). Part 3 supplies drawing, depth and directional shadows (05, 06 and 08). The [complete M2 presentation snapshot](https://github.com/JagerCG/AdaptiveClusterRendering/tree/Qingyao_Tian) also includes the original hierarchy generator, automatic LOD and camera GPU visibility implementation (01, 03 and 04).

## One-minute shadow demo

From the repository root after a Release build, run

```powershell
.\build\Release\AdaptiveClusterRendering.exe --view-config assets/views/m2-shadow-casters.acrview
```

1. Open the **Shadows** tab. A patch outside the camera view casts the shadow in the middle of the receiver.
2. Under **Shadow settings**, leave **Shadow coverage** at **0.30**. Toggle **Cull casters in light view**. The main pass submits 4 clusters and 32 triangles. The light goes from 4 clusters and 32 triangles to 2 clusters and 16 triangles. The receiver keeps its shadow.
3. Switch **Shadows** off and on, then choose **Shadow map (light view)** to see the light's depth. Rotate the camera to show its independence from the light.

## Fixed inputs and geometry

On **Fixed cluster grid (2 x 2)**, select **All clusters**. Its 8 source triangles become 128 at 4 segments per edge. Toggle **Height displacement** and **Wireframe** to see the surface and its edges. **Empty list** clears both geometry and shadows. A saved multilevel ACR can select any of its stored levels through the panel. OBJ imports use the flat M1 partition.

## Verification

```powershell
cmake --preset windows-vs2022 -DACR_ENABLE_GPU_TESTS=ON
cmake --build build --config Release --parallel 1
ctest --test-dir build -C Release --output-on-failure
```

- `scene-stored-levels` and `gpu-hierarchy-roundtrip` validate an authored multilevel ACR, read the resident GPU records and compare rendered depth across a reload.
- `gpu-render-integration` and `gpu-view-replay` cover ordered input, source and generated draws, camera depth and saved views.
- `gpu-shadow-caster-pipeline` compares the full shadow depth image for filtered and unfiltered input. Removing the off-camera caster independently verifies its effect at the receiver. `gpu-shadow-caster-report` checks per-pass counts, timing and OBJ/ACR replay.
- The subdivision and displacement tests validate GPU output positions, UVs, normals, indices, bounds and revision handling for fixed input lists.
