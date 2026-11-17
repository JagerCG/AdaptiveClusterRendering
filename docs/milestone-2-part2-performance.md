# M2 Part 2 performance snapshot

Recorded 2026-11-14 using `scripts/Measure-M2.ps1` on the local Step 7 working
tree. These are measured results from the standalone Part 2 implementation,
not projected performance for the final combined renderer.

## Conditions

- NVIDIA GeForce RTX 4070 Laptop GPU; Windows x64; MSVC 2022 Release build.
- Vulkan API version raw value 4211029; driver version raw value 2559967232;
  64 valid timestamp bits, timestamp period 1 ns. The app did not enable validation.
- Hidden 960 x 640 window; default camera; all source clusters; filled/shaded
  rendering; bounds, cluster colors and UI drawing disabled.
- One setup generation/draw before the batch, then 10 discarded warmup runs and
  50 measured runs per case. Cases ran sequentially. Every generated run forced
  regeneration and output allocation; source-only runs performed no generation.
- Waves uses the built-in 33 x 33 linear height image with amplitude 0.25.
  The n=1 and n=8-off cases have displacement disabled.
- GPU clocks/power, thermals and other desktop workloads were not controlled.
  Treat this as a reproducible exploratory snapshot. Repeat on an idle machine
  for a formal comparison; tiny differences between rows are not an optimization result.

All times below are milliseconds. p95 is the nearest-rank 95th percentile; median
uses the average of the two middle samples when the count is even. No outliers
were removed. Each row links to all 50 raw samples and configuration metadata.

## GPU measurements

| Case / raw report | Drawn triangles | GPU generation median / p95 | GPU generation min-max | GPU render median / p95 | GPU render min-max |
| --- | ---: | ---: | ---: | ---: | ---: |
| [Suzanne source](measurements/2026-11-14/suzanne-source.json) | 3,936 | N/A | N/A | 0.010480 / 0.011168 | 0.009920-0.014144 |
| [Suzanne n=1, off](measurements/2026-11-14/suzanne-n1.json) | 3,936 | 0.015616 / 0.016096 | 0.014112-0.016320 | 0.012496 / 0.013440 | 0.011424-0.013760 |
| [Suzanne n=2, Waves](measurements/2026-11-14/suzanne-n2-waves.json) | 15,744 | 0.041104 / 0.042432 | 0.040128-0.042592 | 0.016672 / 0.017312 | 0.016000-0.017440 |
| [Suzanne n=4, Waves](measurements/2026-11-14/suzanne-n4-waves.json) | 62,976 | 0.098048 / 0.102080 | 0.094176-0.104480 | 0.028624 / 0.029120 | 0.027008-0.065408 |
| [Suzanne n=8, Waves](measurements/2026-11-14/suzanne-n8-waves.json) | 251,904 | 0.299568 / 0.304800 | 0.294304-0.306656 | 0.080688 / 0.101984 | 0.070848-0.103744 |
| [Suzanne n=8, off](measurements/2026-11-14/suzanne-n8-off.json) | 251,904 | 0.267488 / 0.273056 | 0.261056-0.274080 | 0.077248 / 0.156512 | 0.062208-0.176000 |
| [Grid n=8, Waves](measurements/2026-11-14/grid-n8-waves.json) | 512 | 0.018512 / 0.018816 | 0.017696-0.019872 | 0.010352 / 0.010944 | 0.009664-0.015072 |

GPU generation brackets the compute dispatch. GPU rendering brackets the render
pass, including clear, and excludes compute and acquisition/presentation waits.
UI measurements have a different render scope because they also draw ImGui.
These values do not describe complete frame time or FPS.

## CPU latency and generated memory

| Case | CPU generate + wait median / p95 (ms) | Output allocation (bytes) |
| --- | ---: | ---: |
| Suzanne source | N/A | 0 |
| Suzanne n=1, off | 1.216800 / 1.428900 | 425,088 |
| Suzanne n=2, Waves | 1.221450 / 1.333400 | 1,700,352 |
| Suzanne n=4, Waves | 1.468800 / 1.750000 | 6,801,408 |
| Suzanne n=8, Waves | 2.297350 / 2.474100 | 27,205,632 |
| Suzanne n=8, off | 2.255700 / 2.538300 | 27,205,632 |
| Grid n=8, Waves | 1.234100 / 1.383500 | 55,296 |

CPU latency includes planning, reallocation, worklist upload, submission and GPU
queue waits. It is not a CPU subdivision implementation, so dividing it by GPU
dispatch time would not be a meaningful CPU/GPU speedup. The source and worklist
allocations are additional to the output: Suzanne has 117,232 source bytes and
15,744 worklist bytes, plus a 12,288-byte height-image allocation in these runs.
Output stores three 32-byte vertices and three 4-byte indices per generated
triangle, so the output size follows the n-squared growth exactly.

The n=8 Waves case demonstrates 755,712 GPU-generated vertices / 251,904 triangles
within the current 128 MiB output budget. The recorded CPU latency motivates later
buffer reuse and scheduling work; the pass measurements alone do not establish
an end-to-end bottleneck or improvement. General UV/normal seam handling on an
imported model remains a separate limitation; use the continuous-UV fixtures for
the displacement visual demonstration.

## Reproduce

```powershell
cmake --build --preset release --parallel 1
.\scripts\Measure-M2.ps1 -Warmup 10 -Samples 50 -OutputDirectory build/benchmarks/repeat
```

The raw reports are preserved under `docs/measurements/2026-11-14/`. New script
runs also export a CSV under their output directory. See the
[demo guide](milestone-2-part2-demo.md#reproducible-measurement) for the exact timing
scopes, null-field semantics, UI/CLI differences and integration limits.
