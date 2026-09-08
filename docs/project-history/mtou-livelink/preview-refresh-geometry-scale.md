# Preview Refresh Geometry-Scale Measurements

Date: 2026-09-08
Issue: #41
Record type: Reproducible synthetic corpus and bounded performance conclusion

## Scope and decision

The measured supported corpus now includes a 27,744-triangle garment with a
147,744-triangle full-character Driver, independently tested with 4, 32 and
64 Driver Morphs. Every recorded refresh produced usable actor-owned transient
output with verified geometry, normalized skin weights and complete expected
Morph position deltas. This is an observed corpus, not a universal supported
maximum, a production-asset equivalence claim, or a latency guarantee.

**A separate responsiveness follow-up is warranted.** The large geometry takes
1.66–1.72 seconds warmed even with only four Morphs, and 2.99–3.07 seconds with
64 Morphs. These are synchronous Game Thread waits in the real public refresh,
including readiness commit and display selection. The dominant observed stage
is mesh build plus Morph work. First profile mesh construction and final rebuild
separately from projection at the same large 4/64-Morph endpoints, then evaluate
bounded progress/cancellation behavior with all-or-nothing readiness and explicit
refresh preserved. This measurement ticket authorizes neither asynchronous
rebuilding nor cancellation implementation and creates no new child issue.

## Host and repeatable method

- Stock Unreal Editor **5.7.4**, changelist **51494982**, Win64 Development Editor;
  Intel Core i7-12700KF (12 cores / 20 logical processors), approximately 32 GiB
  installed RAM; Windows host; VS 2022 toolchain 14.44.35228 and SDK 10.0.22621.0.
- Plugin 0.5.0 development; `ToolsLab.uproject` loads this checkout's two modules.
  No engine changes, production dependencies, private assets or Maya run are
  involved. Another existing Editor session remained open, so these numbers
  describe that host load, not a dedicated idle-machine laboratory.
- Existing `MtoULiveLink.Editor.Preview.RefreshBenchmark`, using
  `FMtoUPreviewPreparation::RefreshActor` on an EditorPreview-world Binding actor.
  Each case is built deterministically before timing. Each refresh actually
  rebuilds, commits readiness and selects the Generated Preview for display.
- Nine **separate sequential Editor processes**, one selected case each: run 0
  is `process_first_refresh`; runs 1–3 are `warmed`. Run 0 is a cold **refresh
  path**, not a cold operating-system/DDC cache or Editor-startup duration.
  Fixture creation has already exercised asset loading/building before run 0.
  There is no competing benchmark process. A default unfiltered matrix is also
  available; only its first case is process-first, later run-0 rows are correctly
  labelled `case_first_refresh` and must not be reported as independent cold runs.
- GC occurs outside the timer before every refresh. On warmed runs the actor
  retains its previous committed output until refresh begins. Output readback,
  assertions, fixture creation, GC and logging are outside the timed interval.
- Stage boundaries come from the existing public callback. `geometry_resolution`
  includes conversion, source selection and alignment; `weight_transfer` includes
  Closest/Inpaint and quality evaluation; `build_and_morph` includes correspondence,
  skeletal mesh construction, Morph writes and final asset rebuild. The old
  diagnostic's "Morph projection" timer covers that same broad build-and-Morph
  work: it is **not** a pure projection-only CPU profile. Closest/Inpaint kernel
  timers are retained verbatim in each JSON row's diagnostics and summarized below.
- Each run emits one condensed JSON record containing logical fixture name,
  grid parameters, input/output counts, selection, projected/skipped Morphs,
  sparse volume, stage/total time, readiness, output-check result and memory.
  Passing requires host test success **and** all output assertions; Editor exit
  code alone is insufficient because UE can exit zero after an Automation failure.

From a shell with the stock Engine binaries/build scripts on PATH, from the
repository root (substitute local paths outside committed records):

```powershell
Build.bat UnrealEditor Win64 Development unreal/ToolsLab.uproject -WaitMutex -NoHotReloadFromIDE
$cases = 'Small','Representative','Library64','GeometryMedium4','GeometryMedium32','GeometryMedium64','GeometryLarge4','GeometryLarge32','GeometryLarge64'
foreach ($case in $cases) {
    UnrealEditor-Cmd.exe unreal/ToolsLab.uproject -unattended -nop4 -nosplash -NullRHI -DDC-ForceMemoryCache "-MtoUBenchmarkCase=$case" '-ExecCmds=Automation RunTests MtoULiveLink.Editor.Preview.RefreshBenchmark' '-TestExit=Automation Test Queue Empty' "-log=Issue41-$case.log"
}
```

An unknown case fails the test. Omitting `-MtoUBenchmarkCase` runs all nine cases
with the same four samples per case; this is the normal integration regression.

## Deterministic inputs and output oracle

Small retains the stock SkeletalCube geometry and five uniform Morphs. The
original Representative geometry and region Morphs are unchanged. `Library64`
is the former misleadingly named `ProductionScale`: the same 40-vertex,
72-triangle Preview with 60 added uniform Morphs. Its small geometry stays
available as a Morph-library baseline.

The new grids reuse the existing full-character fixture's transforms and slots:
body, face, hair, arm, and two garment boxes. Each closed box has `12*cells^2`
triangles. Medium uses 10 garment cells and 15 other-part cells per edge; Large
uses 34 and 50. The body/face/hair/arm contribute four other-part boxes, and the
garment contributes two. All bones use full weight on bone 0 in these fixtures.
The source upper garment has two slots and the lower garment a third. New cases
explicitly select `Garment_Upper_A`, `Garment_Upper_B`, `Garment_Lower` through
Manual override, matching the selection style of historical C01. Tiny baselines
retain Auto. Density and Morph count never change the spatial transforms.

An early unaccepted prototype used the box generator's face groups for the body
and Auto selection; it selected 57,744 triangles and was rejected at 2.08x source
mass. Body groups were normalized to the single Body group and the benchmark's
large-input selection fixed to the three garment slots. That failed prototype
is excluded from performance tables; no production selection threshold changed.

All full-character libraries start with GarmentFlare, ArmRaise, FaceBlink and
HairSway. The last three have no selected Preview surface and must be absent
from output. Additional `BenchMorph00..` entries translate all Driver vertices
by `(0.1*(i+1), 0.05*(i+1), 0.02*(i+1))` in asset units. GarmentFlare translates
vertices in the upper garment's bounds expanded by 2 units by `(2,0,0)`; this
box also contains some lower-garment vertices. Bounds and positions come from
the stock fixture, with no random seed, asset import or private input.

Every successful run checks the complete position multiset and triangle count
against the input, reads the generated mesh through Geometry Scripting, checks
all skin weights for valid bones and normalization (bone 0 for full-character
fixtures), and checks the exact expected Morph names and **every render vertex's
position delta**, including expected zeros, using the analytic uniform/box
oracle. Sparse indices must be unique and valid and position/normal deltas finite.
UE may emit zero-position, normal-only deltas at the GarmentFlare boundary; sparse
counts include them. The tables report source MeshDescription vertex counts;
sparse deltas index built render vertices, whose count can differ after mesh
build. Sparse volume must not be inferred by multiplying source vertex counts.
The Generated Preview must be transient, outered to the actor, committed in its
usable readiness, and actually selected on the displayed SkeletalMeshComponent.
All 36 records below passed these checks; their final state is Warning (including
three intentionally skipped Morphs on full-character cases), not Error.

| Case | Driver vertices / triangles | Preview and output vertices / triangles | Driver Morphs | Projected / skipped | Sparse deltas |
| --- | ---: | ---: | ---: | ---: | ---: |
| Small | 24 / 12 | 24 / 12 | 5 | 5 / 0 | 120 |
| Representative | 120 / 216 | 40 / 72 | 4 | 1 / 3 | 30 |
| Library64 | 120 / 216 | 40 / 72 | 64 | 61 / 3 | 2,430 |
| GeometryMedium4 | 6,612 / 13,200 | 1,204 / 2,400 | 4 | 1 / 3 | 784 |
| GeometryMedium32 | 6,612 / 13,200 | 1,204 / 2,400 | 32 | 29 / 3 | 34,468 |
| GeometryMedium64 | 6,612 / 13,200 | 1,204 / 2,400 | 64 | 61 / 3 | 72,964 |
| GeometryLarge4 | 73,884 / 147,744 | 13,876 / 27,744 | 4 | 1 / 3 | 8,776 |
| GeometryLarge32 | 73,884 / 147,744 | 13,876 / 27,744 | 32 | 29 / 3 | 397,276 |
| GeometryLarge64 | 73,884 / 147,744 | 13,876 / 27,744 | 64 | 61 / 3 | 841,276 |

## Timing evidence

Milliseconds. Three warmed samples are reported individually below, not selected
best cases. Summary values use their median and full min–max range.

| Case | First refresh total | Warmed total median [range] | Closest first / warmed range | Inpaint first / warmed range |
| --- | ---: | ---: | ---: | ---: |
| Small | 4.734 | 3.174 [3.106–5.115] | 0.074 / 0.019–0.025 | 0.241 / 0.114–2.122 |
| Representative | 5.405 | 4.405 [4.261–4.465] | 0.093 / 0.030–0.035 | 0.337 / 0.184–0.195 |
| Library64 | 15.982 | 14.686 [14.651–17.139] | 0.083 / 0.031–0.033 | 0.313 / 0.168–1.911 |
| GeometryMedium4 | 61.028 | 57.303 [57.291–57.317] | 0.249 / 0.163–0.206 | 4.008 / 2.206–2.252 |
| GeometryMedium32 | 96.956 | 95.566 [95.217–97.210] | 0.224 / 0.161–0.201 | 2.489 / 2.130–3.921 |
| GeometryMedium64 | 145.967 | 142.528 [140.791–143.467] | 0.158 / 0.176–0.251 | 2.196 / 2.202–2.261 |
| GeometryLarge4 | 1675.919 | 1678.547 [1657.147–1721.979] | 1.247 / 0.808–1.393 | 16.426 / 15.463–15.986 |
| GeometryLarge32 | 2281.385 | 2185.560 [2166.279–2191.140] | 1.611 / 0.918–0.985 | 17.912 / 14.997–16.013 |
| GeometryLarge64 | 2970.779 | 3039.505 [2985.341–3071.107] | 1.371 / 1.389–2.427 | 16.736 / 15.380–17.690 |

Detailed callback intervals; C = process-first refresh, W1–W3 = warmed.
Begin/preflight are separate intervals shown together; totals include all
intervals, with only rounding differences.

| Case / sample | Begin / preflight | Geometry + resolution | Weight transfer | Build + Morph | Validation + commit + display | Total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Small / C | 0.066 / 0.004 | 0.899 | 0.405 | 3.236 | 0.124 | 4.734 |
| Small / W1 | 0.043 / 0.003 | 0.331 | 2.207 | 2.421 | 0.110 | 5.115 |
| Small / W2 | 0.042 / 0.003 | 0.286 | 0.204 | 2.530 | 0.108 | 3.174 |
| Small / W3 | 0.041 / 0.004 | 0.311 | 0.201 | 2.443 | 0.105 | 3.106 |
| Representative / C | 0.054 / 0.004 | 1.113 | 0.512 | 3.587 | 0.135 | 5.405 |
| Representative / W1 | 0.047 / 0.003 | 0.653 | 0.278 | 3.386 | 0.099 | 4.465 |
| Representative / W2 | 0.036 / 0.004 | 0.591 | 0.302 | 3.234 | 0.094 | 4.261 |
| Representative / W3 | 0.037 / 0.003 | 0.641 | 0.295 | 3.328 | 0.101 | 4.405 |
| Library64 / C | 0.052 / 0.004 | 0.971 | 0.478 | 14.355 | 0.120 | 15.982 |
| Library64 / W1 | 0.046 / 0.003 | 0.590 | 2.003 | 14.378 | 0.118 | 17.139 |
| Library64 / W2 | 0.039 / 0.003 | 0.594 | 0.271 | 13.631 | 0.113 | 14.651 |
| Library64 / W3 | 0.044 / 0.005 | 0.597 | 0.264 | 13.668 | 0.108 | 14.686 |
| GeometryMedium4 / C | 0.069 / 0.004 | 9.959 | 4.996 | 45.793 | 0.207 | 61.028 |
| GeometryMedium4 / W1 | 0.043 / 0.003 | 9.495 | 3.105 | 44.495 | 0.176 | 57.317 |
| GeometryMedium4 / W2 | 0.038 / 0.004 | 9.339 | 3.084 | 44.631 | 0.195 | 57.291 |
| GeometryMedium4 / W3 | 0.036 / 0.004 | 9.318 | 3.070 | 44.696 | 0.179 | 57.303 |
| GeometryMedium32 / C | 0.069 / 0.005 | 9.381 | 3.411 | 83.898 | 0.193 | 96.956 |
| GeometryMedium32 / W1 | 0.039 / 0.003 | 8.716 | 4.799 | 83.431 | 0.222 | 97.210 |
| GeometryMedium32 / W2 | 0.039 / 0.004 | 8.978 | 3.404 | 82.949 | 0.191 | 95.566 |
| GeometryMedium32 / W3 | 0.038 / 0.004 | 8.864 | 2.936 | 83.154 | 0.221 | 95.217 |
| GeometryMedium64 / C | 0.058 / 0.004 | 9.490 | 3.095 | 133.104 | 0.216 | 145.967 |
| GeometryMedium64 / W1 | 0.043 / 0.003 | 8.551 | 3.194 | 128.787 | 0.213 | 140.791 |
| GeometryMedium64 / W2 | 0.039 / 0.004 | 9.313 | 3.122 | 130.791 | 0.199 | 143.467 |
| GeometryMedium64 / W3 | 0.038 / 0.004 | 8.892 | 3.125 | 130.253 | 0.217 | 142.528 |
| GeometryLarge4 / C | 0.060 / 0.017 | 100.789 | 25.506 | 1548.840 | 0.706 | 1675.919 |
| GeometryLarge4 / W1 | 0.077 / 0.004 | 109.005 | 25.450 | 1586.813 | 0.629 | 1721.979 |
| GeometryLarge4 / W2 | 0.058 / 0.004 | 100.426 | 24.161 | 1531.855 | 0.644 | 1657.147 |
| GeometryLarge4 / W3 | 0.042 / 0.004 | 100.364 | 23.997 | 1553.502 | 0.638 | 1678.547 |
| GeometryLarge32 / C | 0.057 / 0.006 | 103.022 | 27.456 | 2150.219 | 0.626 | 2281.385 |
| GeometryLarge32 / W1 | 0.047 / 0.004 | 100.470 | 23.730 | 2066.259 | 0.630 | 2191.140 |
| GeometryLarge32 / W2 | 0.040 / 0.004 | 100.282 | 23.593 | 2041.722 | 0.638 | 2166.279 |
| GeometryLarge32 / W3 | 0.039 / 0.004 | 101.604 | 24.707 | 2058.527 | 0.679 | 2185.560 |
| GeometryLarge64 / C | 0.079 / 0.004 | 106.635 | 25.835 | 2837.449 | 0.777 | 2970.779 |
| GeometryLarge64 / W1 | 0.046 / 0.003 | 105.616 | 26.023 | 2938.757 | 0.662 | 3071.107 |
| GeometryLarge64 / W2 | 0.047 / 0.004 | 106.196 | 26.799 | 2851.603 | 0.692 | 2985.341 |
| GeometryLarge64 / W3 | 0.039 / 0.004 | 107.076 | 25.424 | 2906.345 | 0.617 | 3039.505 |

## Memory evidence and limitations

All values are MiB (2^20 bytes), from `FPlatformMemory::GetStats`, sampled directly
before/after refresh. `Used` is process resident physical memory; its signed
change is **not attributable allocation**, a leak test, live UObject bytes, or
peak scratch memory. It includes allocator retention, old/new outputs, engine
work and unrelated activity. Allocations can be freed before the after sample.
GC reduces abandoned scratch across samples but cannot reset allocator caches.

`Peak` is the absolute process-lifetime physical-memory high-water mark before
and after refresh, **not a resettable per-refresh peak**. Fixture creation and
previous output validation can establish it. A flat peak cannot prove zero
allocation; peak-minus-baseline cannot identify exact refresh-owned peak bytes.
Fresh processes isolate earlier benchmark cases but still include Editor startup
and fixture creation. These measurements establish the observed process envelope,
not an allocation improvement or a guarantee of flat memory as geometry/Morphs grow.
An allocation attribution claim would require a separate scoped memory trace.

| Case / sample | Used before | Used after | Signed change | Process peak before | Process peak after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Small / C | 2340.6 | 2342.9 | 2.3 | 2461.1 | 2461.1 |
| Small / W1 | 2343.1 | 2343.6 | 0.5 | 2461.1 | 2461.1 |
| Small / W2 | 2343.8 | 2343.8 | 0.0 | 2461.1 | 2461.1 |
| Small / W3 | 2343.9 | 2343.9 | 0.0 | 2461.1 | 2461.1 |
| Representative / C | 2345.7 | 2347.6 | 1.9 | 2465.8 | 2465.8 |
| Representative / W1 | 2347.8 | 2347.8 | 0.0 | 2465.8 | 2465.8 |
| Representative / W2 | 2347.9 | 2347.9 | 0.1 | 2465.8 | 2465.8 |
| Representative / W3 | 2348.0 | 2348.0 | 0.0 | 2465.8 | 2465.8 |
| Library64 / C | 2359.3 | 2361.4 | 2.0 | 2474.7 | 2474.7 |
| Library64 / W1 | 2361.7 | 2361.9 | 0.2 | 2474.7 | 2474.7 |
| Library64 / W2 | 2362.1 | 2362.3 | 0.2 | 2474.7 | 2474.7 |
| Library64 / W3 | 2362.4 | 2362.4 | 0.1 | 2474.7 | 2474.7 |
| GeometryMedium4 / C | 2379.1 | 2386.6 | 7.5 | 2467.2 | 2467.2 |
| GeometryMedium4 / W1 | 2386.9 | 2389.4 | 2.5 | 2467.2 | 2467.2 |
| GeometryMedium4 / W2 | 2389.6 | 2392.2 | 2.6 | 2467.2 | 2467.2 |
| GeometryMedium4 / W3 | 2392.2 | 2394.5 | 2.3 | 2467.2 | 2467.2 |
| GeometryMedium32 / C | 2392.7 | 2406.4 | 13.7 | 2466.6 | 2466.6 |
| GeometryMedium32 / W1 | 2406.7 | 2410.5 | 3.7 | 2466.6 | 2466.6 |
| GeometryMedium32 / W2 | 2410.6 | 2415.2 | 4.7 | 2466.6 | 2466.6 |
| GeometryMedium32 / W3 | 2415.3 | 2416.6 | 1.4 | 2466.6 | 2466.6 |
| GeometryMedium64 / C | 2391.3 | 2410.9 | 19.5 | 2466.0 | 2466.0 |
| GeometryMedium64 / W1 | 2411.3 | 2418.7 | 7.4 | 2466.0 | 2466.0 |
| GeometryMedium64 / W2 | 2418.8 | 2422.9 | 4.1 | 2466.0 | 2466.0 |
| GeometryMedium64 / W3 | 2423.2 | 2429.1 | 5.9 | 2466.0 | 2466.0 |
| GeometryLarge4 / C | 2660.6 | 2701.8 | 41.2 | 2724.6 | 2724.6 |
| GeometryLarge4 / W1 | 2703.3 | 2725.1 | 21.8 | 2724.6 | 2735.7 |
| GeometryLarge4 / W2 | 2719.5 | 2740.5 | 21.0 | 2735.7 | 2751.3 |
| GeometryLarge4 / W3 | 2736.7 | 2748.4 | 11.7 | 2751.3 | 2759.1 |
| GeometryLarge32 / C | 2684.6 | 2829.9 | 145.3 | 2716.8 | 2857.6 |
| GeometryLarge32 / W1 | 2830.4 | 2885.6 | 55.1 | 2857.6 | 2913.3 |
| GeometryLarge32 / W2 | 2874.8 | 2901.3 | 26.5 | 2913.3 | 2929.1 |
| GeometryLarge32 / W3 | 2890.8 | 2909.2 | 18.4 | 2929.1 | 2937.0 |
| GeometryLarge64 / C | 2772.5 | 2986.9 | 214.5 | 2772.5 | 3038.5 |
| GeometryLarge64 / W1 | 2987.4 | 3054.8 | 67.4 | 3038.5 | 3106.7 |
| GeometryLarge64 / W2 | 3038.9 | 3072.8 | 34.0 | 3106.7 | 3124.7 |
| GeometryLarge64 / W3 | 3056.9 | 3080.1 | 23.1 | 3124.7 | 3132.0 |

For the Large64 first refresh, Used rises by 214.4 MiB, while its warmed endpoint
changes are 23.2–67.4 MiB. Absolute process peaks reach 3,132.0 MiB (about 3.06 GiB).
These are concrete resident-memory observations; none is labelled an exact
refresh allocation peak. The larger fixtures expose meaningful process growth
that the original kilobyte-scale inputs could not characterize.

## Historical comparison and supported envelope

[Issue #22's C01 record](model-preview-full-character-recalibration.md) used
`SK_C01_Clothes_09_All` and `SM_C01_Clothes_09`: 27,630 resolved garment triangles
out of a 150,772-triangle Driver, 27 projected + 82 skipped Morphs, 5,516 sparse
deltas, and approximately 1.3–1.7 seconds refresh. The new Large corpus matches
that **order of geometry magnitude**; it does not reproduce the C01 topology,
weights, materials, region count, Morph sparsity, library, or measurement seam.
Its up to 841,276 sparse deltas are far denser. C01 was not rerun here and there
is no valid cross-run optimization percentage. No private asset or path is stored.

[Issue #34's original results](preview-refresh-optimization.md) remain a historical
same-input optimization comparison on tiny geometry. The present small baselines
still take milliseconds, while even the large four-Morph case takes seconds.
Geometry size therefore matters independently of Morph-library size, and adding
Morphs further increases the measured wait. Neither the old 4–17 ms figures nor
the single-root, same-topology synthetic corpus supports a broad responsiveness
claim for arbitrary production garments, retopology, layered surfaces, many-bone
inpaint, Auto selection, other hardware, rendering-enabled interaction or Topia.

## Acceptance checks

- Stock Development Editor build: succeeded, no compiler/linker warnings.
- Nine isolated RefreshBenchmark processes: 9/9 test invocations passed; 36/36
  first/warmed refresh samples passed output checks. The default nine-case
  matrix also passed before the final evidence/readback assertions were added;
  the final Editor regression below rechecks that default path.
- `Automation RunTests MtoULiveLink.Editor`: **27/27 passed**, including all
  Preview and GarmentSurface shared-fixture consumers, lifecycle/failure checks,
  RefreshEndsSession, and the final default benchmark matrix (36/36 output checks).
- `python tools/package_unreal_plugin.py MtoULiveLink --engine 5.7 --json`:
  passed dry-run, 32 files, no release archive created.
- `git diff --check`: passed. Engine-created config additions are removed from the working tree; no engine
  paths, private assets, logs, binaries, caches or release archives are committed.
- No Maya, protocol, Runtime-wide, Topia, repository-wide or release-wide rerun:
  the owning change is the Unreal Editor benchmark and its shared test fixtures.

Stock Editor logs include existing asynchronous asset compilation memory-estimate
messages and synthetic-mesh degenerate-tangent/near-zero-binormal warnings.
Automation reports success; these fixture shading warnings are not a visual
production sign-off. An initial unnecessary render-thread flush caused a link
failure; removing it avoided adding a RenderCore dependency. Early failed fixture
and Morph-oracle probes were corrected before the final successful measurements.
