# MtoU Live Link history

Start with the [product manual](../../../composite/MtoULiveLink/README.md) for
current use and the [changelog](../../../composite/MtoULiveLink/CHANGELOG.md)
for version changes. Records below describe their named acceptance baselines;
their test results do not automatically apply to later code.

## Milestones and current work

| Milestone | Accepted scope and source |
| --- | --- |
| 0.4.0, 2026-09-03 | Integrated through [PR #36](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/pull/36), followed by owner C01/Topia verification. See [stock-engine acceptance](stock-engine-acceptance.md) and [Topia acceptance](topia-engine-acceptance.md). |
| 0.5.0, 2026-09-22 | Owner accepted practical use; local tag `composite-mtou-livelink-v0.5.0`, implementation checkpoint `278e16a001037a08933e8f180c82899a2e567e8e`. No installation packages or additional host-version certification were produced. |
| Preview workflow, 2026-10-13 | [Parent acceptance #47](issue-47-preview-workflow-acceptance.md) covers four delivered workflows and three bounded prototypes on `codex/mtou-preview-workflow`; prototype integration remains a separate task. |

The [joint integration proposal](../../../composite/MtoULiveLink/docs/preview-workflow-integration-plan.md)
is tracked in [Issue #56](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/56).
Read that issue for current decisions and progress, and the
[workflow scope](../../../composite/MtoULiveLink/docs/preview-workflow-roadmap.md)
for product direction.

## Architecture and host baselines

| Topic | Record |
| --- | --- |
| Architecture through the 0.4.0 baseline | [Architecture](architecture.md) |
| Stock Unreal production validation | [Stock-engine acceptance](stock-engine-acceptance.md) |
| Topia build and editor loading | [Topia acceptance](topia-engine-acceptance.md) |

## Character mapping and composition

| Topic | Record |
| --- | --- |
| Complete Maya hierarchy and tiny-scale handling (#44) | [Complete-skeleton acceptance](complete-skeleton-acceptance.md) |
| Importer hash-suffix mapping (#46) | [Hash-suffix mapping acceptance](hash-suffix-mapping-acceptance.md) |
| Primary Driver and additional parts (#45) | [Character-part acceptance](character-parts-acceptance.md) |
| Required-bone mapping with real C01 inputs (#55) | [C01 acceptance](issue-55-real-c01-acceptance.md) |

## Preview generation and quality

| Topic | Record |
| --- | --- |
| Transient geometry and lifecycle (#11) | [Skin Preview acceptance](transient-skin-preview-acceptance.md) |
| Cross-topology Morph transfer (#12) | [Morph Transfer acceptance](preview-morph-transfer-acceptance.md) |
| Garment-only quality thresholds (#13) | [Quality calibration](model-preview-quality-calibration.md) |
| Full-character thresholds (#22) | [Full-character recalibration](model-preview-full-character-recalibration.md) |
| Refresh optimization (#34) | [Optimization](preview-refresh-optimization.md) |
| Geometry-scale performance limits (#41) | [Scale measurements](preview-refresh-geometry-scale.md) |

## Playback and editor workflow

| Topic | Record |
| --- | --- |
| Cache session identity and reconnect (#37) | [Reconnect acceptance](cached-playback-reconnect-acceptance.md) |
| Cached exit and explicit refresh (#38–39) | [Exit/refresh acceptance](cache-exit-and-refresh-review-acceptance.md) |
| Unreal Details and diagnostics (#48) | [Details acceptance](issue-48-ue-details-acceptance.md) |
| Range capture and UE-owned playback (#49) | [Cache workflow acceptance](issue-49-cache-workflow-acceptance.md) |
| Pause, seek and loop (#50) | [Interactive playback acceptance](issue-50-interactive-playback-acceptance.md) |
| Original/Generated garment comparison (#51) | [Comparison acceptance](issue-51-garment-comparison-acceptance.md) |

## Bounded prototypes

The [parent acceptance](issue-47-preview-workflow-acceptance.md) owns the
combined #47 conclusion. Each prototype record owns its scope, limits and
evidence; follow those links for raw host results and reproduction inputs.

| Topic | Record |
| --- | --- |
| UE camera and Sequencer to Maya (#52) | [Camera acceptance](issue-52-camera-sync-acceptance.md) |
| UE level geometry reference in Maya (#53) | [Scene-reference acceptance](issue-53-scene-reference-acceptance.md) |
| Independent prop and second character (#54) | [Multi-subject acceptance](issue-54-multi-subject-acceptance.md) |

## Source preservation

These archives preserve original comments for provenance; current status lives
in the tracker and accepted conclusions in the topic records above.

- [#52 source comments](issue-52-comment-archive.json)
- [#53 source comments](issue-53-comment-archive.json)
- [#54 source comments](issue-54-comment-archive.json)
- [#56 source comments](issue-56-comment-archive.json)
- [#57 source comments](issue-57-comment-archive.json)

Earlier per-change chronology remains in
[the Git version before index consolidation](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/blob/b0f9f3530def60b3d837d7af07aff766789285c8/docs/project-history/mtou-livelink/README.md).
Retention and future additions follow the [history rules](../README.md).
