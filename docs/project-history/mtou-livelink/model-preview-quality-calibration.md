# Model Preview Quality Calibration

Issue #13's engineering deliverables were completed on 2026-08-26 against the
official stock Unreal Engine 5.7.4 Win64 editor (build 5.7.4-51494982). Refresh
now measures a symmetric Driver/Preview surface-distance metric and the
weight-transfer low-confidence ratio, evaluates them against corpus-calibrated
thresholds, and reports a Ready, Warning, or Error verdict with measured reason
text in the preview diagnostics. Structural failures (skeleton, space, scale,
unreadable source data, missing influence bones, transient build errors,
Morph-generation errors) remain hard failures regardless of quality score; the
partial-Morph-intersection yellow warning and zero-intersection
`PREVIEW_MORPH_MISMATCH` error from Issue #12 are unchanged. Visual-envelope
sign-off items that this issue deliberately leaves to the human acceptance gate
are listed at the end; Issue #13 closes on this measurement and automation
evidence together with those explicit deferrals.

## Measurement definition

The symmetric surface distance samples, for every Preview LOD0 vertex, the
distance to the nearest Driver-surface triangle and, for every Driver vertex,
the distance to the nearest Preview triangle. Min, Max, Average, and RMS are
taken over all samples and normalized by the Driver bounding-box diagonal.
The misalignment gate uses the Average so that local missing-surface holes
(few far samples) stay distinct from globally shifted inputs (all samples
far); Max is reported for diagnostics. The inpaint ratio is the share of
Preview vertices without a direct `FTransferBoneWeights` match.

## Corpus

Corpus inputs are identified by stable logical name plus SHA-256 content hash;
no production binary, credential, or machine-specific absolute path is stored.
The production Driver is the imported garment FBX listed below. The paired
production Preview Static Mesh is not a committed binary either: external
Preview assets are identified by their logical name plus content hash at
measurement time and supplied to the harness through the `MTOU_QUALITY_PREVIEW`
asset path, so each measured pair stays reproducible without storing assets.
The measurements below derive every Preview variant deterministically from the
Driver itself, which makes the recorded rows exactly reproducible from the
Driver hash and the variant recipes alone.

| Logical name | Content | SHA-256 |
| --- | --- | --- |
| `stock-SkeletalCube` | stock `/Engine/EngineMeshes/SkeletalCube` driver with five deterministic corrective/directional/stress Morphs | engine asset, unmodified base |
| `SK_C01_Clothes_09` | production garment import source FBX, 20,953,376 bytes | `5DC8A4EBF79F1080C33B02C3CE455BA51B0CD52E01ED717723D7EAFD91FDC7D6` |

Each case derives four variants from one Driver/Preview pair: same-topology
copy, one-triangle local retopology, poked double-layer shell at 90% scale
with split seam positions, and a two-bounds-diagonal translation as the
intentionally misaligned negative input. The committed automation harness
(`MtoULiveLink.Editor.Preview.QualityCorpus`) reproduces every measurement
from these recipes; it accepts optional `MTOU_QUALITY_DRIVER` /
`MTOU_QUALITY_PREVIEW` asset paths to measure external garments without code
changes. The stock corpus runs carry the five-Morph library (representative
corrective, positive/negative and left/right directional pair, and stress
Morph), so Morph count and sparse-delta volume are captured per row. The
production rows measure zero Morphs only because the headless Interchange
import does not carry BlendShapes; production Morph volume for this garment
is retained from the Issue #12 external acceptance (99 projected Morph
Targets, 313,749 sparse deltas, 4 no-surface skips). Measurements were
captured headless (`-NullRHI`) on an i7-12700KF workstation; timings are
single-run feasibility evidence.

## Measured results and threshold derivation

Production garment `SK_C01_Clothes_09` (86,644 LOD0 vertices / 150,772
triangles):

| Case | Vertices | Inpaint ratio | Distance avg | Distance max | Total refresh | Status |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| SameTopology | 86,644 | 0.26532 | 0.000000 | 0.000000 | ~2.8 s | Ready |
| LocalRetopology | 86,645 | 0.26528 | 0.000000 | 0.000000 | ~2.9 s | Ready |
| DoubleLayerSeams | 173,290 | 0.00000 (fallback) | 0.003719 | 0.053707 | ~4.8 s | Warning |
| MisalignedNegative | rejected | — | 1.773360 | 2.000000 | ~1.1 s | Error |

Stock diagnostic fixtures (five-Morph library: representative corrective,
positive/negative pair, left/right pair, stress):

| Case | Vertices | Inpaint ratio | Distance avg | Morphs (sparse deltas) | Status |
| --- | ---: | ---: | ---: | --- | --- |
| SameTopology | 24 | 0.66667 | 0.000000 | 5 (120) | Warning |
| LocalRetopology | 25 | 0.64000 | 0.000000 | 5 (125) | Warning |
| DoubleLayerSeams | 50 | 0.64000 | 0.004681 | 5 (250) | Warning |
| MisalignedNegative | rejected | — | 1.711325 | rejected | Error |

The half-missing-surface regression fixture from Issue #12 measures distance
average 0.2474 and must remain a warning. Calibrated boundaries follow
directly from these anchors:

- `MaxReadyInpaintRatio = 0.27` — just above the largest approved production
  measurement (0.26532), so approved same-topology and retopology revisions
  are Ready while the degenerate diagnostic cubes (0.64–0.67) warn.
- `MaxWarningInpaintRatio = 0.70` — above every valid measured input including
  the worst diagnostic fixture (0.66667); no valid corpus case exceeds it.
- `MisalignedNormalizedDistanceAverage = 0.60` — a conservative round boundary
  inside the measured gap between the extreme acceptable hole (0.2474) and the
  smallest observed misalignment distance average (1.7113); it sits below the
  gap's log-scale midpoint (~0.65) to reject global misalignment earlier.

Boundary behavior is proven by `MtoULiveLink.Editor.Preview.QualityBoundaries`,
which checks results immediately above and below each calibrated boundary.
Threshold values live next to this rationale in the preparation header and are
recalibrated only from new corpus measurements.

## Weight-transfer configuration

V1 keeps the Issue #11 fixed choice: build with `InpaintWeights` (search radius
5% of target bounds diagonal, 30-degree normal threshold, layered-mesh support,
one smoothing iteration at strength 0.5) after comparing against
`ClosestPointOnSurface`. The production-scale double-layer case exposed an
engine limitation: the public inpaint quadratic-programming solve fails with a
handled ensure at ~173k layered vertices, so Refresh falls back to the already
computed closest-point weights and raises a yellow warning instead of failing
the transaction. Closest-point transfer alone matched all vertices there, which
is why the fallback row reports ratio 0.00000. No advanced transfer settings
are exposed in V1.

## Morph-library volume

Morph count and sparse-delta volume are captured per corpus row above; the
stock rows carry the full five-Morph library through projection. The
production rows measure zero Morphs only because the headless Interchange
import does not carry BlendShapes; production Morph volume for this garment
is retained from the Issue #12 external acceptance: 99 projected Morph
Targets, 313,749 sparse deltas, 4 no-surface skips. Morph name/count/delta
stability, intersection handling, and stress cases remain covered by the
deterministic fixtures.

## Timing expectation

Practical expectation from the corpus runs: a production garment of this size
completes Refresh in roughly 3 seconds for approved revisions, under 5 seconds
with the double-layer fallback, and rejects misaligned inputs in about 1 second
before any mesh build. Whole-process editor peaks stayed under 5 GiB physical
across sequential cases. These are workstation-class single-run figures for
planning, not release gates.

## Remaining human sign-off

Silhouette, thickness, penetration, seam continuity, and representative
corrective review of dense-seam and double-layer garments, plus final modeler
and TA visual sign-off, belong to the Issue #14 acceptance gate. This record
establishes the calibrated metrics, thresholds, reproducible corpus evidence,
and automated boundary coverage those reviews build on.
