# Preview Morph Transfer Acceptance

Issue #12 was technically verified on 2026-08-23 against the official stock
Unreal Engine 5.7.4 Win64 editor. Refresh maps each Preview LOD0 vertex to the
nearest Driver triangle, stores barycentric coordinates, and projects every
Driver Morph delta through that correspondence. The implementation uses public
Dynamic Mesh, Geometry Scripting, Skeletal Mesh, and distance-query APIs only;
it uses no private header, copied engine implementation, engine modification,
persistent intermediate asset, or `.uasset` fallback.

## Fixed feasibility input

The deterministic editor fixture starts from the stock `SkeletalCube` Driver,
pokes one Preview triangle to change local topology, and adds a reversed inner
shell at 90% scale. The result is a 50-vertex, 28-triangle double-layer Preview
with split positions representing dense UV seams. The Driver has five Morphs:

| Morph | Driver delta | Verified result |
| --- | --- | --- |
| `Corrective` | +2.0 X | +2.0 X within 0.001 |
| `CorrectiveNegative` | -2.0 X | -2.0 X within 0.001 |
| `Left` | +1.5 Y | +1.5 Y within 0.001 |
| `Right` | -1.5 Y | -1.5 Y within 0.001 |
| `Stress` | +0.75 Z | +0.75 Z within 0.001 |

All five names are generated with 250 sparse deltas. Every generated LOD0
render vertex receives a finite delta, including same-position seam duplicates
and both thickness layers. The representative corrective is checked at weights
0, 0.5, and 1.0 and produces the expected linear 0, +1.0 X, and +2.0 X
progression.

One focused headless editor run measured:

| Vertices | Triangles | Low confidence | Closest | Inpaint | Morph projection |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 50 | 28 | 32 | 0.090 ms | 0.301 ms | 3.055 ms |

The complete focused editor process peaked at 2,362.99 MiB working set and
2,399.36 MiB private memory. These are whole-process headless-editor peaks,
not incremental plugin allocations or release thresholds. Timing and memory
measurements are feasibility evidence only; calibrated production expectations
remain owned by Issue #13.

## Streaming and lifecycle result

Protocol-v4 automation covers empty, partial, and full intersections. The
partial stress session uses three Generated Morphs and a three-name Maya
manifest: two names are accepted and one belongs only to another outfit. A
single frame carries two bone transforms plus accepted values 0.25 and 0.75 and
the other-outfit value 0.9. Live Link and the displayed component apply only
0.25 and 0.75; the non-accepted Generated Morph remains zero. Model diagnostics
report Maya total 3, Generated total 3, accepted 2, Maya-only 1, and UE-only 1.
The partial result is yellow, the full 3/3 result is Ready, the empty result is
rejected with `PREVIEW_MORPH_MISMATCH`, and bone-only remains orange and
excluded from model acceptance.

Disconnect removes the old Live Link Subject so a later outfit cannot reuse a
stale accepted-curve layout. Repeated Refresh preserves Morph names, counts,
and sparse-delta volume while GC releases the previous transient library. An
invalid required Morph fails the Skeletal Mesh Build stage, discards the whole
result, hides the component, and releases the prior library.

## Result and remaining boundary

The deterministic geometry, progression, directional-pair, seam, thickness,
stress, transaction, timing, and memory evidence passes the Issue #12 technical
Go/No-Go gate. This record does not claim production-garment calibration. The
external corpus, visual envelope, quality thresholds, and final sign-off remain
Issue #13.
