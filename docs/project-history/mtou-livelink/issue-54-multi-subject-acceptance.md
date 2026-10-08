# Issue #54: independent prop and second-character preview

Date: 2026-09-29. A bounded, editor-only **two-subject realtime** prototype was
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
| Necessary-bone identity | The 2026-10-08 round replaced the whole-table bone-count rule with #55's shape: every declared bone must exist in the target, carry the declared parent and have only declared bones above it, so no driven bone hangs under an undriven one. Target bones the declaration does not drive keep their reference pose and are reported per subject as `undriven_bones`, so a target with legitimate extra branches (the real C01 case) can pair while the evidence never implies a whole-skeleton match. Declaring a bone the target lacks, or dropping a bone a declared child needs, is still `skeleton_mismatch`. |
| Session identity | Every `frame` and `remove` carries the session it was negotiated under. After a renegotiation the receiver refuses the previous session's frames and removes with `session_mismatch`, applies nothing and leaves the serial untouched; the receiver's own evidence attributes each applied frame to its session. |
| Evaluated time | The serial is the evaluation identity and increases strictly inside a session; the Maya source time may move backwards or repeat. `time_direction` (`first`/`forward`/`backward`/`hold`) is recorded per applied frame on both hosts, so a reverse scrub and a re-edit are visible instead of looking like forward playback. |
| Drive ownership and exit | Each target's prior driver, the writers the preview muted (with whether each belongs to a saved asset) and how it left are reported by `MtoUMultiSubject.Ownership` and in `drive_ownership[]`. A target that had no animation driver is put back into its reference pose on exit and on per-subject removal (`restored_to_reference_pose`), so it no longer keeps the preview's last pose. |
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

The 2026-09-29 review's gaps that were *fixable in the prototype* are closed here;
the real-asset pairing of #54's first gap is not, and stays open. The prototype
still does not touch MtoULiveLink v9, the single-Subject product, the cache or the
animation delivery flow.

| Gap from the review | What changed | Where it is exercised |
| --- | --- | --- |
| Inbound frames carried no session, so old frames after a renegotiation were not isolated | `frame` and `remove` repeat the negotiated session; a mismatch is `session_mismatch`, applies nothing, and does not advance the serial | `MtoUMultiSubjectPrototype.SessionAndTimeIdentity`, Maya host check "a frame from a previous session is refused", both hosts' `session` fields in the evidence |
| "Maya requires increasing source time, UE only checks serial" — reverse scrub, same-frame re-edit and looping had no defined semantics | The serial is the evaluation identity and increases strictly inside a session; the source time is unconstrained and each applied frame records `first`/`forward`/`backward`/`hold` | `SessionAndTimeIdentity` streams times 1 → 3 → 2 → 2; Maya `--times 1,3,2,2` run against the stand-in and the real receiver |
| `SetLocalEvalDisabled` is invisible in the Sequencer UI | Drive ownership is reported per target (prior driver, muted writers with `saved_asset`, exit state) through `MtoUMultiSubject.Ownership` and `drive_ownership[]`. This is a report, not a Sequencer UI affordance; the product still needs one | `DriverExitOwnership`, `drive_ownership[]` in the evidence |
| A component with no previous driver kept the preview's last pose after exit | Such a target is restored to its reference pose, recorded as `restored_to_reference_pose` | `DriverExitOwnership` (arms target: streamed pose, then reference pose after `Stop`) |
| Whole-table bone equality would re-reject real C01's extra branches | Necessary-bone negotiation with reported undriven bones (see the acceptance table) | `FixtureAndAnchorRules`: a declaration that drops the `Hand_L` leaf is accepted with one undriven bone; dropping `Forearm_L` is refused |

Open from the same review, unchanged: a real paired-asset closed loop in a
disposable scene of the Backups project with per-object error and viewport
evidence, and a product-level Sequencer ownership affordance.

## Product boundary

The 2026-09-29 review of `5b825055316ede3cf22efc12e5c7f183db4ce02b`,
preserved as comment `5883765955` in [the source archive](issue-54-comment-archive.json),
accepted the bounded fixture evidence and reran the pure protocol tests. Its
prototype-fixable gaps — inbound old-frame isolation after renegotiation,
same-frame edits, reverse seeks, and exit with no previous animation driver — are
closed by the 2026-10-08 round above; that review's conclusion is superseded for
those points only. The issue stays open for real paired assets.

#47 owns a combined integration decision after #52–54 have the agreed runnable
prototype evidence, support limits and outstanding-work descriptions. The
necessary-bone mapping this prototype now exercises is the candidate migration
contract for #55's accepted shape; adopting it in the product is still #47's and
#55's decision, not this prototype's.

Reports: [开发交付 dev-01](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-6053453464),
archived with its earlier sources in [the source archive](issue-54-comment-archive.json).
Implementation commit: `84f034ff0c57c7801db03b985eb5a1e0e79746de` (local, not pushed
at the time of reporting). This round's evidence lives outside the checkout in
`../.tmp/mtou-issue54/fix-20261008/`: `host-post.json`, `accept-c01.json`,
`ue-prop/` (full suite plus `report/index.json`), `ue-arms/`, `ue-times/` and the
disposable `backups-copy/` shell around the real Backups content. The current
handoff is still comment `5883765955`; it is refreshed at the next
consolidation, as with the 2026-10-08 rounds on #52 and #53.

The product still has one Maya `_CharacterScene`/`_StreamingSession` root and
snapshot, one UE Binding Actor/`MtoU_Character` SubjectKey, one pending frame,
and one cache owner. A product implementation would require an explicit pair
registry and independent subject lifecycle, set-wide frame/time admission and
errors, reversible owner-controlled Sequencer takeover, and a **new versioned**
cache contract and both adapters' conformance tests. This verification neither
commits to that implementation nor authorizes applying the prototype's
`SetLocalEvalDisabled` approach to saved productions: its suppression is
invisible in the Sequencer UI and needs an explicit user-facing ownership state.
