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
| Root motion and placement | The Maya root is an evaluated **world** pose (includes reference ancestors); non-root joints remain parent-relative. Maya measured an animated session parent on a referenced prop at source frames 1 and 3 and verified one parent application against the DAG world matrix. Unreal applied each root under a distinct rotated, off-origin actor anchor once and measured component-space and world-root residuals. Attaching that already-world-rooted target to a moving UE socket is explicitly refused; a later socket design would need to subtract the shared parent instead of applying it twice. |
| Full body versus arms | A full-body description under the arms ID/root is refused with `skeleton_mismatch`, without applying a pose. A fresh `init` for the distinct seven-bone arms skeleton is accepted and four frames apply at the character's source times; there is no implicit hot swap. The receiver retains the deliberate refusal in `errors[]` even after the successful negotiation. |
| Exclusive Sequencer preview and exit | The synthetic Level Sequence contains a **bound** skeletal animation track and is opened in the editor Sequencer. The track moves the character before takeover; its non-serialized local evaluation flag stops it from writing the same component while MtoU drives the pose. Removing the character restores only its track while prop remains live. On exit the earlier pose, animation mode, prop single-node asset/Morph, track flag, sequence sections and package dirty state match their pre-preview state. The fixture uses transient assets; no saved production sequence was altered. |
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

## Product boundary

The 2026-09-29 review of `5b825055316ede3cf22efc12e5c7f183db4ce02b`,
preserved as comment `5883765955` in [the source archive](issue-54-comment-archive.json),
accepts the bounded fixture evidence and reran 21 pure protocol tests; host and
build results were inspected from existing reports. The current issue remains
open for real paired assets and identity/time/exit evidence. Inbound old-frame
isolation after renegotiation, same-frame edits, reverse seeks/loops, and exit
with no previous animation driver are not established by the four-frame
fixtures. An undriven component may retain its last preview pose after exit.
These gaps must remain distinct from the deferred product implementation. The
record reconciliation on 2026-09-30 did not rerun host checks.

#47 owns a combined integration decision after #52–54 have the agreed runnable
prototype evidence, support limits and outstanding-work descriptions. Product
reuse of #55's necessary-bone mapping is a candidate migration contract; the
prototype's exact whole-skeleton comparison does not yet establish it.

The product still has one Maya `_CharacterScene`/`_StreamingSession` root and
snapshot, one UE Binding Actor/`MtoU_Character` SubjectKey, one pending frame,
and one cache owner. A product implementation would require an explicit pair
registry and independent subject lifecycle, set-wide frame/time admission and
errors, reversible owner-controlled Sequencer takeover, and a **new versioned**
cache contract and both adapters' conformance tests. This verification neither
commits to that implementation nor authorizes applying the prototype's
`SetLocalEvalDisabled` approach to saved productions: its suppression is
invisible in the Sequencer UI and needs an explicit user-facing ownership state.
