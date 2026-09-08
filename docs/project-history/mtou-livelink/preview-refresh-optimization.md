# Preview Refresh Optimization

Date: 2026-09-03
Status: Accepted for MtoU_LiveLink 0.4.0 follow-up (Issue #34)
Record type: Durable performance conclusion; method is repeatable via automation.

## Scope

Scope clarification from Issue #41 (2026-09-08): all timing and allocation
conclusions below concern the original tiny geometry only. The historical
`ProductionScale` name meant Morph-library scale (40 vertices / 72 triangles),
not production geometry. It is now named `Library64` in the benchmark. These
historical measurements remain unchanged. For substantial geometry, seconds-long
waits and the current follow-up decision, see
[geometry-scale measurements](preview-refresh-geometry-scale.md).

Issue #34 required evidence-backed Preview refresh improvements that preserve
transactional behavior: reuse projection data and scratch storage, remove
avoidable full-mesh copies, keep the explicit refresh boundary, and add
progress or cancellation only with justifying evidence.

## Method

New automation `MtoULiveLink.Editor.Preview.RefreshBenchmark` builds three
deterministic fixtures and logs identical JSON per scale: vertices, triangles,
projected/skipped Morph counts, sparse deltas, Closest/Inpaint/Morph-projection
stage milliseconds, total refresh milliseconds, and process Used/Peak physical
memory. Baseline and optimized runs use identical Driver, Preview revision,
Morph library, and output validation on stock Unreal Editor 5.7.4 Win64
(`-NullRHI -DDC-ForceMemoryCache`).

- Small: stock SkeletalCube Driver with 5 uniform Morphs, same-topology Preview
  (24 vertices, 12 triangles, 5 projected, 120 sparse deltas).
- Representative: full-character Driver with garment-only Preview
  (40 vertices, 72 triangles, 1 projected + 3 no-surface skips, 30 sparse).
- ProductionScale: identical Representative geometry with a 64-Morph library
  (61 projected + 3 skips, 2,430 sparse), matching the production order of
  Morph-library size without copying production assets into the repository.

## Results

Warmed, stable values (one cold-start baseline Small run measured 13.446 ms
Morph / 30.3 ms total and is recorded as a warm-up outlier, not the comparison
point):

| Scale | Baseline Morph / total | Optimized Morph / total |
| --- | --- | --- |
| Small | 2.531 ms / 6.6 ms | 2.2–2.6 ms / 4.0–5.0 ms |
| Representative | 3.168 ms / 5.5 ms | 3.0–3.2 ms / 5.1–5.2 ms |
| ProductionScale | 16.475 ms / 17.3 ms | 14.4–15.5 ms / 15.4–16.9 ms |

Per-scale output validation is identical before and after: vertex, triangle,
Morph, skipped, and sparse-delta counts match exactly, with usable Warning
readiness on these synthetic fixtures. Process Used/Peak memory stays flat
within run-to-run noise because the synthetic meshes are kilobytes against a
2.3 GiB editor process; the structural peak reduction is exact: per refresh,
transient `UDynamicMesh` objects N→1, full `FDynamicMesh3` copies N→1, and
dense zeroed arrays N→1 with selective clearing of only touched Driver points.

## Conclusions

- The limiting ProductionScale fixture refreshes ~10–12% faster with no
  material change on smaller fixtures.
- Generated geometry, skin weights, Morph names/deltas, display composition,
  diagnostics, and readiness outcomes are equivalent: all 49 `MtoULiveLink`
  Automation tests pass, including MorphProjection, MissingMorphSurface,
  Lifecycle, InvalidGeometry, FullCharacter, MeshData, and QualityCorpus.
- Failed refresh still commits no partial Generated Preview; explicit refresh
  and actor-owned transient readiness are unchanged.
- Progress/cancellation was not added: total synchronous refresh waits are
  4–17 ms on these fixtures, so a more complex execution model is not
  justified by evidence.
- No new production dependency, persistent asset, background auto-generation,
  or multi-character/LOD framework was introduced.

Linked from #28 and #34.
