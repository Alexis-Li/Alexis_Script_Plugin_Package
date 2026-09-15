# MtoU_LiveLink Development History

MtoU_LiveLink 0.4.0 is the completed Maya 2022.4 and stock Unreal Editor 5.7.4
baseline. It is merged to `main`, accepted, and closed without formal release
packages; subsequent development uses version 0.5.0.

The timeline records the status at each historical date; its earlier pending
gates do not override later acceptance. For the 0.5.0 development follow-ups,
use the scoped acceptance records below and the project's
[Unreleased changelog](../../../composite/MtoULiveLink/CHANGELOG.md#unreleased).
Those records do not establish a complete 0.5.0 production or release gate.

## Timeline

- **2026-07-31:** Defined the one-character local Live Link architecture,
  protocol, Maya sampling contract, Unreal binding workflow, lifecycle, and
  performance gates.
- **2026-08-11:** Completed the Maya sender, Unreal runtime/editor modules,
  automated tests, bilingual product documentation, and independent host
  packaging.
- **2026-08-11:** Corrected shared BlendShape alias handling to match the
  established FBX/Unreal workflow, then passed stock-engine production
  acceptance with the supplied C04 rig and exported Skeletal Mesh.
- **2026-08-11:** Migrated both host components into the `composite/` category,
  verified ToolsLab external-plugin discovery, and centralized product metadata
  at the composite project root.
- **2026-08-12:** Released the protocol-v2 outfit-aware Maya workflow, Chinese
  status and diagnostics UI, exact scene-rate sampling, bidirectional
  BlendShape warnings, and the single Binding Actor connection rule.
- **2026-08-13:** Consolidated connection negotiation, the cross-host protocol
  contract, Maya character-scene ownership, and Maya streaming-session
  lifecycle behind four focused interfaces without changing installation or
  user workflow.
- **2026-08-19:** Implemented and locally verified protocol v3 bind-pose
  capture and source-bind-to-target-reference pose mapping, removing the need
  to connect from frame 1 or an A Pose. Production-scene acceptance remains
  pending for this revision.
- **2026-08-20:** Repaired protocol v3 bind-pose capture against production
  rigs: read the downstream joint-to-dagPose bind connection, fall back to the
  setup-time pose for joints without stored bind data, and resolve
  skin-cluster bind-matrix conflicts from the joint-connected dagPose instead
  of rejecting the skeleton.
- **2026-08-21:** Implemented Issue #5 Cached Playback: mutually exclusive
  Maya modes, incremental temporary cache storage, timeline-safe capture,
  ordered no-drop protocol-v3 replay, repeat replay, lifecycle cleanup, and
  deterministic cached-session tests. The C01 stock-engine production gate
  remains a separate external acceptance task.
- **2026-08-21:** Completed Issue #6 capture safety: cancellation races,
  timer and disk-usage failures, recapture during replay, exact range
  finalization, owned-path validation, stale-cleanup filtering, and controller
  cache-state cleanup are covered by deterministic tests. Maya 2022 host tests
  pass; the external C01 stock-engine production gate remains pending.
- **2026-08-21:** Completed Issue #7 Cached Playback lifecycle hardening:
  manual stop and repeat replay preserve ordered delivery, transport failure
  retains a completed cache for compatible reconnect, disconnect and
  incompatible lifecycle paths delete owned files, and stale cached-session
  events cannot affect a newer controller state. Deterministic tests cover the
  terminal and cleanup paths; the external C01 stock-engine production gate
  remains pending.
- **2026-08-23:** Passed the Issue #11 stock-UE 5.7.4 transient Skin Preview
  gate with public Geometry APIs, same-topology and local-retopology fixed
  inputs, transactional actor-owned lifecycle automation, and no persistent
  asset fallback.
- **2026-08-23:** Completed the Issue #12 technical Preview Morph Transfer gate
  on stock UE 5.7.4 with cross-topology barycentric projection, accepted-only
  v4 streaming, double-layer/seam stress automation, and transactional transient
  lifecycle evidence.
- **2026-08-25:** Implemented specification #16 upload-then-play Cached
  Playback on protocol v5: complete cache upload with Unreal-side validation
  and Ready gating, Unreal-driven local replay at the captured scene rate,
  exactly-once in-order application, stable playback-performance failure,
  replay-again reuse of the uploaded cache, and per-session transient cache
  ownership. The external C01 stock-engine production gate (321 frames applied
  by Unreal within 5% of the captured rate) remains pending.
- **2026-08-25:** Hardened Cached Playback on protocol v6 with authoritative
  character revisions, identity-scoped upload/play outcomes, truthful accepted
  completion evidence, bounded transient resources, and a lossless return to
  Real-time Preview.
- **2026-08-26:** Calibrated Model Preview quality warnings (Issue #13) on the
  stock fixtures plus the external C01 production garment: symmetric surface
  distance and inpaint-ratio metrics with measured Ready/Warning/Error
  boundaries, misalignment rejection before mesh build, closest-point fallback
  when the public inpaint solve fails on large layered inputs, and a
  reproducible corpus measurement harness.
- **2026-08-26:** Clarified Model Preview negotiation for outfits without
  BlendShapes (Issue #20): a zero-name Maya outfit manifest is intentionally
  bone-driven and negotiates Ready with an empty accepted Morph set, while a
  non-empty manifest with zero generated matches keeps blocking with
  `PREVIEW_MORPH_MISMATCH`. Protocol v6 framing and message shapes are
  unchanged.
- **2026-08-28:** Compiled both plugin modules and loaded MtoU_LiveLink in the
  Athena editor with Topia Engine 5.7.4 without modifying engine or host-project
  source/configuration files. Added a repeatable dry-run-first helper for the
  same isolated build and binary-install workflow.
- **2026-08-29:** Completed Issue #24 by moving Driver garment-surface
  resolution behind the private `MtoUDriverGarmentSurface` module: Preview
  preparation consumes one resolution outcome, the old resolution helpers were
  deleted, focused `MtoULiveLink.Editor.GarmentSurface.Auto/Manual/Failures`
  tests cross the new seam, and the outer Preview tests keep only cross-seam
  integration. Compiled against stock UE 5.7.4 and passed all 37 owning
  `MtoULiveLink` Editor automation tests; the Unreal packaging check passes and
  no generated output is staged.
- **2026-08-29:** Completed Issues #25 and #26 without changing user-visible
  behavior or protocol v6: Preview readiness now has one Binding-actor-owned
  snapshot and transition boundary, while Maya Cached Playback now owns its
  lifecycle behind one action/view seam instead of leaking cache, transport,
  timer, identity, and phase state into the Controller.
- **2026-08-31:** Completed Issue #27 by resolving hidden Driver slots through
  stable current-asset metadata, rejecting unmappable or shared final slots
  transactionally, and isolating Runtime Morph workflow tests from shared
  `/Engine` fixtures. Stock UE 5.7 compilation, all 39 `MtoULiveLink` tests,
  and Unreal packaging validation pass; production visual acceptance remains
  a separate external gate.
- **2026-09-01:** Completed Issue #32 by routing every Auto material-evidence
  triangle through the shared per-triangle final material-slot resolver,
  gating mass accounting on the Preview triangle count, and rejecting
  zero-triangle, degenerate, NaN, and infinite-coordinate geometry
  transactionally at the shared resolution admission. Focused resolver tests
  cover reordered-slot evidence disambiguation and the low-triangle edge
  cases; a preparation test proves invalid geometry commits no partial
  Generated Preview. Stock UE 5.7 compilation, all 43 `MtoULiveLink` tests,
  and the Unreal packaging dry-run pass.
- **2026-09-02:** Passed the complete C01 production acceptance gate on Maya
  2022.4 and stock Unreal Editor 5.7.4, including live-preview resource and
  reconnect checks plus three exact 321-frame Cached Playback cycles. Updated
  the bilingual user workflow and recorded the durable evidence in
  `stock-engine-acceptance.md`.
- **2026-09-03:** Optimized Preview Morph projection scratch reuse, repaired
  renamed Driver material-slot resolution and failed-Refresh display recovery,
  then merged the complete 0.4.0 implementation to `main` through PR #36.
  The merged tree passed both GitHub validation checks, the stock UE 5.7.4
  build, all 51 `MtoULiveLink` Automation tests, Maya pure and host tests,
  protocol and repository validation, scoped Ruff, and both package checks.
  The project owner then manually reverified the complete C01 and Topia paths,
  closing 0.4.0 without producing formal release packages; subsequent
  development advances to 0.5.0.
- **2026-09-15:** Accepted and closed Issue #44 on `main`: Maya now publishes
  every joint plus the intermediate transforms needed to preserve the real DAG
  hierarchy; Unreal preserves finite non-zero tiny scales such as the sample's
  `1e-12` pupil joints. Independent Maya world-matrix reconstruction, strict
  1472-node character negotiation, rendered Animation/Model poses, two Preview
  refreshes, and Animation reconnect completed successfully. The retained
  Preview weight-transfer warning remains a visual review boundary rather than
  a skeleton-connection failure.

## Stable Records

- [Architecture](architecture.md)
- [Stock-engine production acceptance](stock-engine-acceptance.md)
- [Cached Playback reconnect acceptance](cached-playback-reconnect-acceptance.md)
- [Cached exit and explicit refresh review acceptance](cache-exit-and-refresh-review-acceptance.md)
- [Topia-engine build and load acceptance](topia-engine-acceptance.md)
- [Transient Skin Preview acceptance](transient-skin-preview-acceptance.md)
- [Preview Morph Transfer acceptance](preview-morph-transfer-acceptance.md)
- [Model Preview quality calibration](model-preview-quality-calibration.md)
- [Model Preview full-character recalibration](model-preview-full-character-recalibration.md)
- [Preview Refresh optimization](preview-refresh-optimization.md)
- [Preview Refresh geometry-scale measurements](preview-refresh-geometry-scale.md)
- [Complete-skeleton connection and preview acceptance](complete-skeleton-acceptance.md)

## Current Project

- [Product documentation](../../../composite/MtoULiveLink/README.md)
- [Repository rules](../../../AGENTS.md)
- [Composite project rules](../../../composite/AGENTS.md)

The current composite repository standard is owned by the two `AGENTS.md`
files above and is intentionally not duplicated in this history archive.
