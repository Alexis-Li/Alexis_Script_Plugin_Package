# Issue #54: independent prop and second-character preview

Current review: **2026-10-09, changes required; issue remains open**. R-001,
R-002 and R-004 passed the bounded checks below. R-003 and R-005 remain open,
R-006 is a reproduced test-launch defect, and R-007 identifies the existing
real-asset evidence gap. Implementation: `84f034ff0c57c7801db03b985eb5a1e0e79746de`
(local, not pushed). The review ran at
`7efa9f80b577a5f181558f5e295ab45f70b89ed3`; the prototype and its Maya sampling
dependency have no source differences between those baselines.

The original 2026-09-29 bounded, editor-only **two-subject realtime** prototype was
exercised on Maya 2024 and Unreal 5.7.4. It does not change the MtoULiveLink
product, its v9 single-Subject protocol, its cache, or the existing animation
export/import and Sequencer delivery process. The source, runnable recipe and
commands are in [the prototype README](../../../composite/MtoULiveLink/prototypes/multi-subject/README.md);
[protocol.md](../../../composite/MtoULiveLink/prototypes/multi-subject/protocol.md)
records the two-object wire, identity, time and space rules, the **cache-only**
design, the single-Subject product-contract inventory, and a minimal cutover
order. Neither the supplied production Maya scene nor the Backups Unreal project
was modified by this prototype.

## Acceptance

| Criterion | Observed result |
| --- | --- |
| Explicit pairing and strict skeletons | Each `init` maps a full Maya reference-root DAG path and subject ID to one chosen Unreal actor/Skeletal Mesh component. Bone names, parent indices, bind-local poses and Morph names must match that target. The character and independent prop use distinct targets even though both have a bone `Root` and Morph `Shared`. Different-skeleton arms are another target, not Additional Parts or a retarget. |
| Evaluated references and one time | Maya opens two actual `.ma` references, evaluates joints and BlendShape weights at one source-frame time and sends both in one serial/frame. The receiver validates the entire enabled pair before applying it and echoes `session`, `serial`, `time` and per-subject status. `character+prop` and `character+arms` each applied frames 1–4; the two targets held different values for the same Morph name. Removing prop after frame 2 left character streaming; transport disconnect, malformed JSON and editor-world cleanup release session/preview ownership. The Maya session creates no animation curves or expressions and closes its scene without saving either reference. |
| Bone subset identity (R-005 open) | The receiver accepts an ancestor-closed subset of target bones and reports omitted target bones as `undriven_bones`. It does not derive the target's required bones from skinning across LODs or require those bones to be declared. Maya still sends its captured full bone table and the receiver rejects any source bone absent from the target. This is not #55's necessary-bone contract and does not establish C01 compatibility. |
| Session identity | Every `frame` and `remove` carries the session it was negotiated under. After a renegotiation the receiver refuses the previous session's frames and removes with `session_mismatch`, applies nothing and leaves the serial untouched; the receiver's own evidence attributes each applied frame to its session. |
| Evaluated time | The serial is the evaluation identity and increases strictly inside a session; the Maya source time may move backwards or repeat. `time_direction` (`first`/`forward`/`backward`/`hold`) is recorded per applied frame on both hosts, so a reverse scrub and a re-edit are visible instead of looking like forward playback. |
| Drive ownership and exit | The existing no-driver exit checks pass (R-004). Ownership reporting exists, but after renegotiation it can report `reference_pose` while the target is actively previewing: the previous restoration flag is not cleared on takeover (R-003). |
| Cache | No multi-object cache was built. The protocol note specifies one source-time lattice, shared inclusive range/fps, atomic readiness, per-subject failure invalidating the set, and ownership/seek/loop isolation for a later **versioned** product contract. |
## Checks and evidence

| Host/check | Exercised result |
| --- | --- |
| Maya `mayapy tests/maya_host_multi_subject_tests.py --result <scratch>/host-post.json` | 99 checks, zero failures. Includes independent refs and solo-versus-paired poses, authored bind locals, duplicate names/Morph isolation, moving reference parents, negative identity/time/socket tests, per-subject removal, renegotiation, file hashes and scene close. |
| CPython `python -m unittest discover -s <prototype>/maya/MtoUMultiSubjectPrototype/tests -t <prototype>/maya/MtoUMultiSubjectPrototype -v` | 21 wire-contract tests passed. |
| UE `Build.bat UnrealEditor Win64 Development unreal/ToolsLab.uproject -WaitMutex -NoHotReloadFromIDE -NoUBA` | Built with stock UE 5.7.4. |
| UE `UnrealEditor-Cmd unreal/ToolsLab.uproject -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests MtoUMultiSubjectPrototype" -TestExit="Automation Test Queue Empty"` with `-MtoUMultiSubjectMayapy`, `-MtoUMultiSubjectPeer`, `-MtoUEvidence`, `-MtoUMultiSubjectScenario=character-prop`, four frames and `-MtoUMultiSubjectRemove=2` | 6/6 tests passed: strict target/anchor rules, character+prop stream, renegotiation/refusals, real Maya peer, actual opened Sequencer takeover/restore and world cleanup. The `RealMayaPeer` test started mayapy itself and used a loopback socket into the actual UE receiver. |
| Same UE peer test, `-MtoUMultiSubjectScenario=character-arms`, four frames | 1/1 passed. Expected first `skeleton_mismatch` and fresh successful negotiation are both recorded; Maya `ok=true`, Unreal cumulative `ok=false` because the refusal is retained, not because the subsequent four-frame preview failed. Exit reports `preview_active=false`, `preview_writers_suppressed=false` and `session_end_reason=disconnect`. |
| Repository gates | `python tools/validate_repository.py`: `ok: True`; `python -m unittest discover -s tests -v`: 22 tests, 21 passed and 1 host PowerShell-dependent test skipped. |

Evidence JSONs from this run live outside the checkout in
`../.tmp/mtou-issue54/evidence/`: `host-post.json`,
`ue-final/report/index.json`, `ue-final/maya-peer-character-prop/`,
and `ue-arms-verified/report/index.json`,
`ue-arms-verified/maya-peer-character-arms/`. The sender and receiver reports
record exact subject IDs, source times, serials, applied statuses, joint rows,
Morph weights and error codes. In the first character+prop frame both IDs
applied at source time 1: `Shared` was 0 for character and 1 for prop; arms at
time 1 held 0.5 with character still at 0. Across the observed four-frame
sessions the largest recorded per-bone/root-world discrepancy was below
0.001 in the prototype's transform-delta metric; frame-by-frame numbers are in
the UE JSON, rather than a visual or frame-rate guarantee. The sample contains
no pixel/viewport comparison.

The supplied Maya C01 scene was separately probed **read-only** with the
prototype plus a generated prop (`accept-c01.json`): centimetres, 30 fps,
`|Group|root`, and identical file SHA-256/size/timestamp before and after.
That is an input inventory, **not** proof that its production skeleton, its
asset attachments or a saved sequence in the Backups project match the
transient Unreal fixtures. Production asset selection needs an explicit pair
negotiation and visual check before any integration.


## 2026-10-08 round: identity, time, ownership and exit

The 2026-10-08 delivery attempted the prototype-fixable gaps; the independent
2026-10-09 review accepts R-001/R-002/R-004 within tested limits, but not
R-003/R-005. Real-asset pairing also stays open. The prototype
still does not touch MtoULiveLink v9, the single-Subject product, the cache or the
animation delivery flow.

| Gap from the review | What changed | Where it is exercised |
| --- | --- | --- |
| Inbound frames carried no session, so old frames after a renegotiation were not isolated | `frame` and `remove` repeat the negotiated session; a mismatch is `session_mismatch`, applies nothing, and does not advance the serial | `MtoUMultiSubjectPrototype.SessionAndTimeIdentity`, Maya host check "a frame from a previous session is refused", both hosts' `session` fields in the evidence |
| "Maya requires increasing source time, UE only checks serial" — reverse scrub, same-frame re-edit and looping had no defined semantics | The serial is the evaluation identity and increases strictly inside a session; the source time is unconstrained and each applied frame records `first`/`forward`/`backward`/`hold` | `SessionAndTimeIdentity` streams times 1 → 3 → 2 → 2; Maya `--times 1,3,2,2` run against the stand-in and the real receiver |
| `SetLocalEvalDisabled` is invisible in the Sequencer UI | A console/evidence report was added, but R-003 remains open because it carries a previous session's exit state into active preview. A product UI remains outside this prototype | `DriverExitOwnership` passes the first exit; `SessionAndTimeIdentity` evidence reproduces the stale report |
| A component with no previous driver kept the preview's last pose after exit | Such a target is restored to its reference pose, recorded as `restored_to_reference_pose` | `DriverExitOwnership` (arms target: streamed pose, then reference pose after `Stop`) |
| Whole-table bone equality would re-reject real C01's extra branches | Only target-side subset acceptance was added; necessary-bone coverage and source-extra mapping remain absent (R-005) | `FixtureAndAnchorRules` accepts dropping `Hand_R`, refuses missing `Forearm_L`; all fixture vertices are weighted to root, so it does not test a missing weighted leaf |

Open from the same review, unchanged: a real paired-asset closed loop in a
disposable scene of the Backups project with per-object error and viewport
evidence, and a product-level Sequencer ownership affordance.

## Product boundary

The 2026-09-29 review of `5b825055316ede3cf22efc12e5c7f183db4ce02b`,
preserved as comment `5883765955` in the source archive at Git commit
`7efa9f80b577a5f181558f5e295ab45f70b89ed3`,
accepted the bounded fixture evidence and reran the pure protocol tests. Its
prototype-fixable gaps — inbound old-frame isolation after renegotiation,
same-frame edits, reverse seeks, and exit with no previous animation driver —
passed the bounded 2026-10-09 re-verification. The issue remains open for
R-003/R-005/R-006 and the existing real paired-asset gap R-007.

#47 owns a combined integration decision after #52–54 have the agreed runnable
prototype evidence, support limits and outstanding-work descriptions. The
prototype bone-subset rule is not equivalent to #55's accepted necessary-bone
mapping. Product adoption remains #47's and #55's decision.

Reports: [开发交付 dev-01](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-6053453464),
archived with its earlier sources in [the source archive](issue-54-comment-archive.json).
Implementation commit: `84f034ff0c57c7801db03b985eb5a1e0e79746de` (local, not pushed
at the time of reporting). This round's evidence lives outside the checkout in
`../.tmp/mtou-issue54/fix-20261008/`: `host-post.json`, `accept-c01.json`,
`ue-prop/` (full suite plus `report/index.json`), `ue-arms/`, `ue-times/` and the
disposable `backups-copy/` shell around the real Backups content. The current
handoff is comment `5883765955`, updated with the 2026-10-09 review. The archive
preserves its pre-review text; the earlier review text for the same comment ID
remains in Git at `7efa9f80b577a5f181558f5e295ab45f70b89ed3` in that archive.

## 2026-10-09 independent review

[Formal review review-01](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-6077332601)
contains finding categories, reproduction conditions and the closure decision.

The protocol suite passed 23/23, Maya 2024 host checks passed 102/102, and the
UE 5.7.4 build succeeded. The unmodified full `MtoUMultiSubjectPrototype`
Automation suite passed 8/8 with the real Maya character-prop peer, four frames
and removal after frame 2. Raw evidence is retained beside the Git root under
`../.tmp/mtou-issue54/review-20261009/` for the next implementer; it is local,
not remotely published. No production Maya or Backups assets were opened or
saved in this review. No BaseColor or production playback acceptance is claimed.

| ID | Current finding and completion condition |
| --- | --- |
| R-003 | Reproduced in `ue-prop/session-identity/mtou-multi-subject-character-prop-identity-unreal.json`: preview is active and serials 2/3/4/5 applied, but character reports `restored_to_reference_pose=true` and `exit=reference_pose`. Reset the prior exit flag in TakeOver and prioritize current driving state; test renegotiation and remove/rejoin ownership, not just first exit. |
| R-005 | Source-confirmed contract gap in `DescribeSkeletonMismatch`: only declared bones are checked. Required weighted branches can be omitted, while source-only redundant branches still fail. Derive required target dependencies, map them unambiguously to source, project poses and test weighted missing leaves and harmless source extras. No real C01 deformation failure is claimed as reproduced. |
| R-006 | Reproduced RealMayaPeer launch failure when `-MtoUMultiSubjectTimes=1,3,2,2` precedes `-abslog=<log>`: CommandLineArgumentValue copies the entire suffix; Maya rejects the UE option with exit 2, zero frames and Automation 0/1. Fix token boundaries and argument validation. Moving Times to the final position is a diagnostic workaround, not a fix: the arms peer then passes 1/1 and applies first/forward/backward/hold. The only receiver error in that passing run is the intentional initial `skeleton_mismatch`. |
| R-007 | Existing real-asset evidence gap from dev-01/current handoff. Select explicit C01/UE and independent prop pairs in disposable scenes, cover arms separately, capture common-space motion/Morph/exit and BaseColor evidence without saving originals. If moved to a later issue, record an explicit scope decision rather than silently dropping it. |

The passing suite does not assert the R-003 contradictory fields or the R-005
weighted omission. The R-006 failing UE process returned exit 0 despite the
failed Automation report; report/index.json, not process status alone, is the
acceptance result. Full-suite evidence is in `ue-prop/`; the failed argument
ordering and successful reordered control are in `ue-arms-times/` and
`ue-arms-times-last/`. The review introduced no implementation or test edits.

The product still has one Maya `_CharacterScene`/`_StreamingSession` root and
snapshot, one UE Binding Actor/`MtoU_Character` SubjectKey, one pending frame,
and one cache owner. A product implementation would require an explicit pair
registry and independent subject lifecycle, set-wide frame/time admission and
errors, reversible owner-controlled Sequencer takeover, and a **new versioned**
cache contract and both adapters' conformance tests. This verification neither
commits to that implementation nor authorizes applying the prototype's
`SetLocalEvalDisabled` approach to saved productions: its suppression is
invisible in the Sequencer UI and needs an explicit user-facing ownership state.
