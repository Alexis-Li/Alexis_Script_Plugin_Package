# Transient Skin Preview Acceptance

Issue #11 passed on 2026-08-23 against the official stock Unreal Engine 5.7.4
Win64 editor. The implementation uses the public Geometry Scripting mesh-copy
functions, `FTransferBoneWeights`, Dynamic Mesh attributes, and the public
Skeletal Mesh build path. It uses no private engine header, copied engine
implementation, modified engine build, or persistent temporary-asset fallback.

## Fixed transfer choice

V1 always compares `ClosestPointOnSurface` and `InpaintWeights`, then builds
with Inpaint. Its fixed settings are a search radius of 5% of the target bounds
diagonal, a 30-degree normal threshold, layered-mesh support enabled, one
smoothing iteration, and smoothing strength 0.5. Unmatched/inpainted vertices
are reported as low confidence and produce Warning rather than automatic
failure.

One stock-editor automation run measured:

| Fixed LOD0 input | Vertices | Low confidence | Closest | Inpaint |
| --- | ---: | ---: | ---: | ---: |
| Same topology | 24 | 16 | 0.020 ms | 0.113 ms |
| Local retopology | 25 | 16 | 0.020 ms | 0.121 ms |

These timings record the Issue #11 comparison for follow-up calibration; they
are not quality or performance thresholds. Both fixed inputs build skinned LOD0
previews, and the local-retopology test verifies every vertex has weights and a
representative non-root bone pose moves weighted geometry.

## Lifecycle and persistence result

Editor automation covers the public preparation entry point, five observable
stages, explicit-only Refresh, Dirty invalidation, repeated and failed Refresh,
GC, replacement, actor destruction, level reload, world unload, Reimport,
source rebuild, and the editor-shutdown cleanup seam. Asset Registry, package
flags, and filesystem checks show normal Refresh exposes no persistent asset
and writes no `.uasset`. Mesh checks cover UV channels, normals, tangents and
binormal signs, vertex colors, material slots, empty slots, slot order, and
asset material assignment.

Protocol automation accepts a ready Generated Preview only as a visibly marked
bone-only Model diagnostic when BlendShapes are disabled. BlendShape-enabled
Model negotiation was intentionally unavailable at this gate; Preview Morph
generation and accepted-only streaming are recorded separately in the
[Issue #12 acceptance](preview-morph-transfer-acceptance.md). Final
visual-quality thresholds and production garment sign-off remain outside this
gate by its stated non-goals.
