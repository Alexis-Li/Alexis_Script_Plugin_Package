# MtoU_LiveLink Development History

The current implementation is MtoU_LiveLink 0.4.0, a composite Maya 2022.4 and
stock Unreal Editor 5.7.4 Live Link product. Version 0.4.0 remains unreleased.

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

## Stable Records

- [Architecture](architecture.md)
- [Stock-engine production acceptance](stock-engine-acceptance.md)
- [Transient Skin Preview acceptance](transient-skin-preview-acceptance.md)
- [Preview Morph Transfer acceptance](preview-morph-transfer-acceptance.md)

## Current Project

- [Product documentation](../../../composite/MtoULiveLink/README.md)
- [Repository rules](../../../AGENTS.md)
- [Composite project rules](../../../composite/AGENTS.md)

The current composite repository standard is owned by the two `AGENTS.md`
files above and is intentionally not duplicated in this history archive.
