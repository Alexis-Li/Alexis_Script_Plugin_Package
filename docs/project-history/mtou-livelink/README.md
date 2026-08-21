# MtoU_LiveLink Development History

The current implementation is MtoU_LiveLink 0.3.0, a composite Maya 2022.4 and
stock Unreal Editor 5.7.4 Live Link product. Version 0.3.0 remains unreleased.

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

## Stable Records

- [Architecture](architecture.md)
- [Stock-engine production acceptance](stock-engine-acceptance.md)

## Current Project

- [Product documentation](../../../composite/MtoULiveLink/README.md)
- [Repository rules](../../../AGENTS.md)
- [Composite project rules](../../../composite/AGENTS.md)

The current composite repository standard is owned by the two `AGENTS.md`
files above and is intentionally not duplicated in this history archive.
