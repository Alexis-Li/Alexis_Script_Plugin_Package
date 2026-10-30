# Issue #52: Unreal to Maya camera and Sequencer acceptance

**Accepted and closed on 2026-10-08.** Independent review-02 accepted the bounded
prototype at `25336faee2e03076e7e22e829a9793509b1cfbca`; R-007–R-009 are verified
closed. Tests ran from `d4b8d3155d6f54d17c598b1f1db4cb710f4d3cc0`, whose camera
prototype code is identical. UE C++ is unchanged from the review-01 build at
`91413946466d8f0337104f4bf087b7a4962d3428`; review-02 reused its compiled modules
and does not claim a new build.

The [current handoff](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/52#issuecomment-5888803268)
owns issue status; independent acceptance comes from review-02, original comment
`6055097768`, preserved in the [source-comment archive](issue-52-comment-archive.json). The archival closeout inspected existing evidence
and verified preservation; it did not rerun host acceptance or change scope.

## Accepted scope

| Issue requirement | Effective conclusion and boundary |
| --- | --- |
| Official reuse, adaptation, missing capabilities, license/version | The [official capability record](../../../composite/MtoULiveLink/prototypes/camera-sync/official-capabilities.md) distinguishes Autodesk's Maya→UE camera route from reverse synchronization. Review-01 inspected source, MIT license and listed releases; review-02 reused the unchanged record. No official plugin was installed or certified for UE 5.7. |
| Evaluated camera and parameter mapping | One active perspective Cine Camera provides world transform, focal length, horizontal/vertical FOV, filmback, fit/offset, crop, aperture, focus/DOF, near clip and output/aperture resolution. Maya's gate uses aperture pixel extent. The [mapping record](../../../composite/MtoULiveLink/prototypes/camera-sync/mapping.md) owns conversions and representational differences. |
| Known-marker composition and DOF differences | Actual Maya read-back projection and four Arnold film-fit configurations pass. DOF parameter agreement does not establish cross-renderer image equality. Out-of-frame markers are excluded from visible-pixel evidence. Far clip, overscan and extra bokeh shaping remain explicitly limited. |
| Single time authority, cuts, subframes and restoration | The open Level Sequence editor supplies time/camera. Explicit origins, different rates, non-zero starts, fractional frames, two Camera Cuts and one subsequence are covered. Identity contains time and camera content; stale/duplicate/old-session reports are refused. Stop and failure restore supported camera state, gate, time and undo enablement. |
| Real-time/cache relationship | UE owns camera/time; Maya owns the witness pose. The prototype uses a separate TCP channel and protocol v2. Product v9, packages and production pose transport are unchanged. Cached Playback stays off; shared-clock integration belongs to #47. |
| Reproducible prototype and support boundaries | The [prototype README](../../../composite/MtoULiveLink/prototypes/camera-sync/README.md), fixtures and maintained host tests supply run entry points. Result reports and independent reproductions are retained in the [evidence archive](issue-52-camera-sync-evidence/README.md). |
| Minimal product boundary and decisions | One active perspective camera, explicit origin, subframe evaluation and identity-checked replies are the bounded direction. Shared coordinates/origin, authority switching, pairing, ownership, protocol negotiation and UI remain a unified #47 decision after #52–#54's agreed prototype progress. |

Static, undriven, writable cameras may be borrowed. Animation, constraints,
driven ancestors and protected write attributes are refused before mutation;
use another camera name for a disposable camera and a writable resolution gate.
Orthographic/simultaneous cameras, deeper nested cuts, PIE/packaged execution,
Movie Render Queue resolution discovery and full-character/Morph product
transport are outside acceptance.

## Independent verification

Review-02 executed these checks on Windows, Maya 2024 and stock UE 5.7.4,
with disposable scenes and isolated preferences. C01/Backups production assets
were not opened or modified. Official-source conclusions are historical review.

| Check | Result | Durable evidence |
| --- | --- | --- |
| Mapping under system Python and mayapy | Each interpreter passed 63/63. | Original review-02 in the [comment archive](issue-52-comment-archive.json), maintained tests. |
| Maya host/Arnold | 145 checks, zero failures, including 50 safety checks; four film-fit configurations. Largest compared visible-marker component error 0.196001 px, tolerance 2 px. | [Host result](issue-52-camera-sync-evidence/maya-host.json). |
| Independent review-01 failure inputs | Public disconnect restores time 17, 800×600 gate, camera/socket ownership and original failure reason. Connection refusal preserves disabled undo. Animated borrowing is rejected without key changes or fabricated success; locked borrowing preserves locks/values. | [Result](issue-52-camera-sync-evidence/review-reproduction.json), [portable reproduction](issue-52-camera-sync-evidence/review-02-reproduction.py). |
| UE Automation with both opt-in real peers | 11 succeeded, zero failed/skipped/warned; exit 0. | [Automation report](issue-52-camera-sync-evidence/ue-automation.json). |
| RealMayaPeer | Generation 2 converged; maximum actual read-back marker difference 1.08873e-07 NDC. No anomalous lines, trailing bytes or failed sends. Superseded/synthetic replay refusals are expected negative cases. | [UE peer report](issue-52-camera-sync-evidence/ue-peer.json). |
| EditorLoopFollow | All 13 steps pass; six connection lifetimes restore; ten fractional-time applications. Maximum read-back difference 7.5052e-07 NDC, peer exit 0. | [Editor-loop report](issue-52-camera-sync-evidence/editor-loop-ue.json). |
| Official capabilities and product boundary | Review-01's source/license/version inspection remains applicable to unchanged records. No new official-plugin build or installation claimed. | Archived review-01 and official capability/mapping records. |

The module ticker drives the natural loop: drag, cut, parked-camera editing,
sequence close/reopen, client loss/rejoin and repeated stop/start. Its seconds-long
fixture does not establish sustained playback performance. Real local TCP injects
the supplementary disconnect; callback/hello fault checks also use mocks.
Batch mayapy cannot establish an interactive scriptJob lifecycle or a UE crash
test. The single-joint witness validates the identity join and paused-frame loop,
not full-character same-frame production streaming.

## Findings and remaining ownership

| ID | Category / severity | Current conclusion or completion condition |
| --- | --- | --- |
| R-003 | Independent review action | Complete: review-01 identified concrete failures; review-02 independently verified their fixes. |
| R-007 | Spec / P2 | Verified closed. Run/connect/callback failures clean up unconditionally, preserve the failure reason and restore undo without flushing unrelated history. The old baseline leaked scene/socket state after transport loss. |
| R-008 | Spec / P1 | Verified closed. Driven inputs are refused with `CAMERA_INPUT_DRIVEN`; actual read-back supplies projection/status. `HOST_STATE_MISMATCH` rejects mismatches with rollback. The old baseline reported success after animation overwrote the requested camera. |
| R-009 | Standards / P2 | Verified closed. Protected leaf/compound/transform/gate inputs are refused with `LOCKED_SYNC_ATTRIBUTE`; plugs are never unlocked. The old baseline lost focal and resolution locks. |
| R-001 | Non-blocking acceptance gap | #47 host follow-up: verify interactive idle-pump attachment/removal and repeated lifetime without duplicate scriptJobs. |
| R-002 | Non-blocking acceptance gap | #47 performance follow-up: measure sustained playback latency percentiles and define drop/hold behavior. |
| R-004 | Future candidate | #47 decides common space/origin, authority switching, session/evaluation identity, pairing, ownership, protocol negotiation and UI after #52–#54's agreed progress. |
| R-005 | Future candidate | #47 evaluates ADR 0013's monotonic cache clock before sharing Sequencer time. Cache remains off. |
| R-006 | Future candidate | #47 decides MRQ resolution authority, Maya camera persistence and exit-frame policy. |

Historical R-008 measured a 0.933237 NDC difference from actual read-back while
the requested-value report claimed approximately zero. This is a historical
numerical reproduction, not a rendered error or a current defect. Original
triggers/code references/interim conclusions remain in the comment archive.

## Archive and reproduction

- [Source-comment archive](issue-52-comment-archive.json): exact IDs, authors,
  timestamps and full bodies of the five reports captured before consolidation.
  Review-02 is comment `6055097768`; review-01 is `6051051676`. Earlier snapshots
  remain accessible through the archive's immutable `previous_archive` reference.
- [Acceptance evidence](issue-52-camera-sync-evidence/README.md): seven final
  review result files and the independent review-02 reproduction script, with
  each file's purpose. Superseded runs and the old manifest remain in Git history.
- Use maintained prototype commands for host/Arnold and UE acceptance; both
  real peer arguments must be enabled. A run skipping them is insufficient.
- Supplementary independent reproduction:
  `mayapy <repo>/docs/project-history/mtou-livelink/issue-52-camera-sync-evidence/review-02-reproduction.py <scratch>/review-repro.json`.

Render images are omitted; measured coordinates and errors remain in host JSON,
and maintained tests regenerate them. Archival closeout verifies preserved
content and references; it does not establish new runtime acceptance.
Closing this prototype does not grant product acceptance or implement #47.
