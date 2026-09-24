# MtoU_LiveLink Development History

MtoU_LiveLink 0.5.0 is the owner-accepted milestone as of 2026-09-22, following
practical use with no issues reported. The local Git tag
`composite-mtou-livelink-v0.5.0` freezes this baseline; the implementation
checkpoint before documentation closeout is
`278e16a001037a08933e8f180c82899a2e567e8e`.

This milestone does not produce installation packages, deploy new binaries, or
add host-version certification. Existing scoped acceptance records retain their
limits; owner acceptance is not a claim that every host combination was rerun.
The timeline records the status at each historical date, so earlier development
or pending-release wording does not override this milestone decision. Versioned
changes are recorded in the [changelog](../../../composite/MtoULiveLink/CHANGELOG.md).

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

- **2026-09-20:** Implemented Issue #45 character composition: Primary Driver
  plus Additional Parts share one connection and subject, with combined Morph
  routing and Primary-only garment preparation. Asset compatibility and saved
  level loading fixes are captured in the
  [character-part acceptance record](character-parts-acceptance.md).

- **2026-09-20:** Completed Issue #46 on `main`: connection negotiation accepts
  the hash-suffixed import rename (`<complete short name>_<32 hexadecimal
  digits>`) for a duplicated Maya short name beside the numeric suffix, keeping
  exact-name precedence, the already-mapped-parent rule, candidate uniqueness,
  target reuse rejection, and complete one-to-one coverage. Diagnostics now lead
  with the first unmapped Maya path, its parent, the blocked Maya and unreached
  Unreal counts, and import-rename hints, and they separate confirmed extra
  Unreal bones from unreached ones. The supplied C02 character negotiates
  1502/1502 nodes with exactly its 19 hash renames, four body, head, garment, and
  face poses, and an explicit reconnect on stock Unreal Editor 5.7.4 with Maya
  2024, all within `2.1e-13 cm`. The Model workflow with a Generated Preview was
  not run: the C02 Binding has no Preview Static Mesh and no C02 garment Static
  Mesh exists, so Model coverage stays with the owning Automation tests. Stock UE
  5.7.4 build, all 67 `MtoULiveLink` Automation tests, Maya pure 140/140,
  repository validation, and repository tests pass; evidence in the
  [hash-suffix mapping acceptance](hash-suffix-mapping-acceptance.md).
- **2026-09-20:** Closed the Issue #46 review finding: the extra-versus-unreached
  classification had asked whether an unmatched Unreal bone's parent was
  accounted instead of successfully mapped, so children of a parent reported
  only as a mismatch or an ambiguity were claimed as extra bones. The parent
  test now uses the mapped set, and the negotiation test covers a mismatching
  parent with a blocked descendant and several same-named parents; restoring the
  previous condition fails those assertions. The rebuilt plugin passed all 67
  `MtoULiveLink` Automation tests again.

- **2026-09-21:** Accepted and closed Issue #45 after real C02 cross-host
  playback measurements, owner visual feedback, and fixes for Binding Undo/Redo,
  including Garment Slot Override revision invalidation. Final owning Automation
  passed 71/71. The [acceptance record](character-parts-acceptance.md) owns the
  measured results, remaining limits, and delivery state; closure is not a
  formal 0.5.0 release.

- **2026-09-22:** The owner accepted the current implementation as the 0.5.0
  milestone after practical use without reported problems. Documentation and a
  project-prefixed local tag freeze the baseline without installation packages
  or additional host-version validation. Subsequent work uses a separate
  development branch.

- **2026-09-23:** Completed the real C01 Maya-to-UE technical acceptance for
  Issue #55 with Clothes_12, Head, and Hair: rendered Animation/Model poses,
  transient Preview Refresh and reconnect, plus live and Cached Playback.
  The [acceptance record](issue-55-real-c01-acceptance.md) owns the evidence
  and limits. Independent UE verification then reproduced ambiguous
  source-bone selection; the first correction compared a required bone against
  Maya sources in its mapped parent's scope, with exact names outranking
  importer renames and equal claims refused. The reviewer's three single-target
  reproductions, the new source-mapping regressions, the full 76-test suite,
  and a real C01 Animation/reconnect re-run all pass on the fixed tree. A
  second review round then found that several required targets still resolved
  sources in capture order: a free suffix target split two indistinguishable
  sources across targets, and an exact-versus-rename pair could connect or
  refuse by sibling order. That review kept Issue #55 open.

- **2026-09-24:** Fixed the multi-target source collision with exact-name
  reservation per mapped parent scope and a fail-closed same-parent duplicate
  check. Numeric/hash suffixes, both sibling orders, a public socket
  handshake, live/cache projection, and all 76 UE Automation tests pass in an
  isolated stock UE 5.7.4 project. Maya 2024 and the actual C01 meshes pass
  Animation/reconnect again with 872 required bones. The
  [acceptance record](issue-55-real-c01-acceptance.md) retains the exact
  checks and limits; Issue #55 remains open for final owner acceptance.

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
- [Hash-suffix skeleton mapping acceptance](hash-suffix-mapping-acceptance.md)
- [Character-part composition acceptance](character-parts-acceptance.md)
- [Issue #55 real C01 acceptance](issue-55-real-c01-acceptance.md)

## Current Project

- [Product documentation](../../../composite/MtoULiveLink/README.md)
- [Next-stage preview workflow scope](../../../composite/MtoULiveLink/docs/preview-workflow-roadmap.md)
- [Repository rules](../../../AGENTS.md)
- [Composite project rules](../../../composite/AGENTS.md)

The current composite repository standard is owned by the two `AGENTS.md`
files above and is intentionally not duplicated in this history archive.
