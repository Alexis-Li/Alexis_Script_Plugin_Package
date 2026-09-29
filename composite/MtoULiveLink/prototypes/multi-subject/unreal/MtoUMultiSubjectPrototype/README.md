# MtoU Multi Subject Prototype (Unreal side)

Bounded, editor-only Unreal receiver for the Issue 54 verification: two Maya
objects, each with its own explicit root and its own Unreal target, driven by one
shared frame time, with an explicit preview takeover that can restore the
animation state it replaced.

This is prototype code. It is standalone from the `MtoULiveLink` product module
and from the ToolsLab project, it does not change the product protocol v9, and
it deliberately supports only the fixed profile below: exactly two subjects, no
retargeting, no Additional Parts, and no cache.

## What is verified here

| Requirement | How this prototype covers it |
| --- | --- |
| One explicit root per object, one Unreal target per root | `FMtoUTargetRegistration` pairs an actor anchor with one `USkeletalMeshComponent`; the wire `init` names each subject's `root` and the receiver refuses a declaration whose root is not the first, parentless bone |
| Strict skeleton identity | Bone names, parent names and bone count must match the target skeleton exactly, and every advertised `bind` row must match the target's reference pose within the prototype tolerance (0.25 cm translation, 0.5 deg rotation, 0.005 scale), so a differently resting rig cannot deform on the same skeleton; both are `skeleton_mismatch`. Declared curves must be Morph Targets of the target mesh (`curve_not_in_target`) |
| Two objects on one shared time | One `frame` message carries `serial` + `time` and every enabled subject; the receiver applies a frame only after every subject validated, then answers one `applied` |
| Same-named bones and Morphs stay apart | Each subject owns one target and one pose instance; the character and prop both have a bone named `Root` and a Morph named `Shared`, and the tests assert each target holds only its own values |
| Single-subject removal, disconnect, world close | `remove` disables one subject, restores its prior animation state and re-enables only the writers that belong to that subject, so the other subject keeps streaming; a TCP disconnect and a world cleanup restore both and leave no session |
| Anchor applied exactly once | The Maya root pose is a world pose; the target's placement is the actor anchor only. The tests assert Unreal's `RootWorld == receivedRoot * AnchorTransform` and the preflight refuses any socket or parent motion that would apply the parent twice |
| Existing animation / Sequencer takeover | `SetLocalEvalDisabled` (a flag that is *not* serialized with the Level Sequence) plus a real Sequencer evaluation releases a skeletal animation track; exiting restores the track and the component's animation state, and the tests compare the evaluated pose before, during and after |
| No permanent asset edits | Every fixture asset, actor and Level Sequence is transient; the evidence records that the sequence graph is unchanged and its package is not dirty |
| Machine-readable evidence | `FMtoUMultiSubjectReceiver::SaveEvidence` writes one JSON file per run (schema `mtou-multi-subject-evidence/1`) |

## The sample (shared with the Maya peer)

Two scenarios, both with a full-body `character`:

* `character-prop`: `character` + `prop`
* `character-arms`: `character` + `arms` (a different skeleton, used for the
  mismatch/refusal/renegotiation path)

The authoritative rig recipe is the Maya one
(`maya/MtoUMultiSubjectPrototype/scripts/mtou_multi_subject_recipe.json`); the
Unreal fixture builds the same bone sets, the same parents and the same Morph
on a transient world:

```
character  Root(-1)  Spine(0)  Chest(1)  Head(2)                       cube 14x70x14
prop       Root(-1)  PropBody(0)  PropTip(1)                            cube 26x40x8
arms       ArmsRoot(-1)  UpperArm_L(0)  Forearm_L(1)  Hand_L(2)
                        UpperArm_R(0)  Forearm_R(4)  Hand_R(5)          cube 80x20x20
```

* Every subject declares exactly one curve, `Shared`; `character` and `prop`
  share the bone name `Root`, so same-named bone and Morph isolation is
  measured rather than assumed.
* `Chest` exists only in the full-body rig, so pointing the arms target at the
  full-body input is refused with `skeleton_mismatch`.
* The `init` root is the full Maya DAG path; only its last path/namespace
  element is compared with the first (parentless) declared bone. The recipe's
  paths are `|character:Group|character:Root`, `|prop:Root` and
  `|arms:Socket|arms:ArmsRoot`, so the compared names are `Root`, `Root` and
  `ArmsRoot`. `bind` needs one ten-number tuple per bone; the receiver
  is checked against the target's reference pose (0.25 cm translation, 0.5 deg
  rotation, 0.005 scale). A bone whose rest pose differs is refused with
  `skeleton_mismatch` naming the bone: identical names and parents alone would
  still deform wrongly. The tolerances are prototype noise bounds, not a
  deformation budget; the recipe's advertised bind locals match the fixture.
* Maya-side ancestors (the character's static `Group`, the arms' keyed
  `Socket`) stay inside Maya: the declared root pose is the evaluated world pose
  of the rig, and the Unreal side applies its own actor anchor exactly once.
* Anchor placements on the Unreal side (off-origin and rotated, so an anchor
  applied twice or not at all is visible): character `(500, -275.5, 120.75)` /
  `(0, 35, 0)`, prop `(-275.25, 610.5, 33.75)` / `(12, -40, 7)`,
  arms `(140.5, 300.25, 65)` / `(-18, 9.5, 0)`.
* Scripted Unreal-side sample (the fixtures' own deterministic frames; used by
  the tests that never talk to Maya, source frame `F`, step `s = F - 1`):

| Subject | Bone | Value (UE space, cm) |
| --- | --- | --- |
| character | `Root` | `(100cos(2*pi*s/24), 100sin(2*pi*s/24), 25+2s)`, yaw `5s` deg |
| character | `Spine` | `(0,0,30)`, yaw `10s` deg |
| character | `Head` | `(0,0,15)`, pitch `8s` deg |
| character | other bones | bind local pose |
| character | `Shared` | `0.25 + 0.01s` |
| prop | `Root` | `(0, 30, 10+1.5s)`, yaw `4s` deg |
| prop | `PropBody` | `(0,0,18)`, roll `15s` deg |
| prop | `Shared` | `0.75` |
| arms | `ArmsRoot` | `(0, -20, 5)`, yaw `6s` deg |
| arms | `UpperArm_L` | `(25,0,30)`, pitch `12s` deg |
| arms | `UpperArm_R` | `(-25,0,30)`, pitch `-12s` deg |
| arms | `Shared` | `0.75` |

The wire values are already UE space: Maya applies its own `convert_transform`
(ten numbers per bone: `tx,ty,tz,qx,qy,qz,qw,sx,sy,sz`, centimetres) and the
receiver stores them unchanged.

## Wire profile

`127.0.0.1` TCP, newline-delimited compact JSON, one object per line, UTF-8,
1 MiB per line. Maya is the client.

* `{"type":"init","version":1,"fps":30.0,"subjects":[{"id","root","bones":[{"name","parent"}],"curves":[...],"bind":[[10 numbers],...]}, ...]}`
* `-> {"type":"ready","session":<id>}` after the receiver verified both targets,
  or `{"type":"error","code","details"}`.
* `{"type":"frame","serial":<int>,"time":<sourceFrame>,"subjects":[{"id","transforms":[[10 numbers],...],"curves":[...]}]}`
  carries every enabled subject; `transforms` is one ten-number row per declared
  bone (the same shape as `bind`), `serial` starts at 1 and is strictly
  increasing per connection, and `time` is the one Maya evaluated time of the
  pair.
* `-> {"type":"applied","session","serial","time","subjects":[{"id","status":"applied"|"disabled"}]}`
  after the whole frame was applied (a frame reply lists exactly the subjects
  that frame carried, in negotiated order), or `{"type":"error","code","details"}`.
* `{"type":"remove","id":...}` stops driving exactly that subject, restores its
  prior animation state, and re-enables only the writers that belong to it (a
  session-wide writer stays suppressed until the session ends); the acknowledgement is an `applied` state event
  that echoes the last applied serial/time (0/0.0 when none) and lists every
  configured subject with its current status. Removing the last subject ends the
  session.
* A new `init` is a new negotiation: it is validated first (a refused init
  changes nothing), and a successful one ends the previous session, restores
  it, and starts a new session id. A different target mapping therefore requires
  a new negotiation, never a hot swap.
* Framing errors (`malformed_json` for a line that is not one JSON object,
  `too_large` past 1 MiB) answer once and close the connection. Semantic errors
  keep the connection open for a corrected `init`.

Stable error codes: `version_unsupported`, `subjects_shape`, `message_shape`,
`unknown_subject_id`, `target_reused`, `skeleton_mismatch`,
`curve_not_in_target`, `anchor_conflict`, `target_detached`, `no_session`,
`frame_order`, `frame_subjects`, `frame_shape`, `subject_not_enabled`,
`malformed_json`, `too_large`, `preview_conflict`.

## Running it

Editor console commands (module `MtoUMultiSubjectPrototypeEditor`, plugin
`MtoUMultiSubjectPrototype`):

```
MtoUMultiSubject.Listen [scenario=character-prop|character-arms] [port=54340] [out=<dir>]
MtoUMultiSubject.Stop
MtoUMultiSubject.Evidence [out=<dir>]
MtoUMultiSubject.Peer <mayapy> <peer script> [scenario=] [port=] [frames=] [start-frame=] [remove-at=] [drop-after=] [out=]
```

`Listen` logs `MtoUMultiSubject: listening 127.0.0.1:<port> scenario=<name>
targets=2`; the automation tests use an ephemeral port instead of 54340.

Maya peer command line the receiver expects:

```
mayapy <peer.py> --host 127.0.0.1 --port <n> --scenario character-prop|character-arms \
       --frames N --fps 30 --start-frame 1 --evidence <file> [--remove-at <frame>] [--drop-after <frames>]
```

Exit code 0 means the scenario's expected outcome happened; the peer writes its
evidence JSON whether it succeeded or not.

## Evidence

`SaveEvidence` writes `<out>/mtou-multi-subject-<scenario>-unreal.json` with the
schema `mtou-multi-subject-evidence/1`:

* `ok` — no protocol rejection was recorded, at least one frame was applied,
  and no session is left active. `character-arms` deliberately sends a wrong
  full-body declaration first: its receiver `ok` is false with exactly one
  `skeleton_mismatch`, even when the fresh negotiation and all live frames pass.
  Check `session_end_reason`, `last_error_code` and `errors[]` together.
* `targets[]` — id, anchor actor, component, skeleton signature (names and
  parents), anchor validation result, resolved anchor transform, and the
  animation state the takeover saved.
* `subjects[]` — active negotiated declarations, frame counts and maximum
  measured bone/root-world deltas; this array is empty once the session ends.
  The retained `frames[]` still contain the per-object measurements.
* `frames[]` — per frame: serial, time, `apply_ms`, whether the preview was
  active, and per subject every bone's received local pose, component-space
  pose and delta, the root world position, and the Morph values the component
  holds. The record is bounded (`MaxRecordedFrames`, default 240).
* `preview_writers[]` — writer state, including restored local-mute flags after
  exit. `preview_writers_suppressed` is false once preview has ended.
* `errors[]` — every rejection with its stable code, details and serial.

## Automation tests

`MtoUMultiSubjectPrototype.*` (`EditorContext | EngineFilter`):

* `FixtureAndAnchorRules` — sample skeletons, strict identity (including the
  bind comparison, and its refusal for a wrongly resting rig), anchor-once, and
  the refusal of a socket-attached target.
* `CharacterPropStream` — real socket session: `init`/`ready`, three frames
  applied, anchor-once and pose measurements, same-named bone and Morph
  isolation, single-subject removal with restore, a refused partial frame,
  disconnect cleanup, and the evidence file.
* `NegotiationRefusals` — two ids registering one target, frame before `init`,
  wrong version, three subjects, unknown id, unknown curve, parent mismatch,
  same names/parents with a wrong advertised bind, the `character-arms`
  mismatch-then-renegotiate path, and framing close semantics.
* `SequencerTakeover` — a real Level Sequence opened in the editor Sequencer:
  the track drives the character before the preview, stops writing while the
  preview is active, the removal of the character restores only its own writer
  while the prop keeps previewing, and the pose and animation state return after
  the exit.
* `WorldCleanup` — a session dies with its world, and a cleanup of another world
  is ignored.
* `RealMayaPeer` — opt-in host check:
  `-MtoUMultiSubjectMayapy=<mayapy> -MtoUMultiSubjectPeer=<peer.py> [-MtoUEvidence=<dir>] [-MtoUMultiSubjectScenario=] [-MtoUMultiSubjectFrames=] [-MtoUMultiSubjectRemove=] [-MtoUMultiSubjectDrop=]`.
  Without the flags the test reports that the host check was not requested
  instead of failing.

## Measured limits

* Pose evidence is programmatic (component-space transforms, Morph weights,
  world root positions); this transient sample does not produce viewport or
  rendered-image comparisons. Inspect real assets separately before adoption.
* The preview driver is the prototype's explicit equivalent of independently
  driven Live Link subjects: one `UAnimInstance` subclass per target applies
  the received bone pose, and the target sets its own Morph weights. This
  prototype does not register product Live Link subjects.
* Morph isolation is measured per component: the target calls
  `SetMorphTarget` for its negotiated names and the evidence reads those values
  with `GetMorphTarget`. Animation proxy curves are not the same storage as
  `MorphTargetCurves`. `UPoseableMeshComponent` was not used because it has no
  per-instance Morph Target API.

## Productization risks (input for the integration document)

* The takeover suppresses a writer with `UMovieSceneTrack::SetLocalEvalDisabled`,
  which is not serialized but is also invisible in the Sequencer UI. A user could
  believe a track is live while a preview owns the object; the product decision
  (and the UI affordance) is not implemented here. The prototype never calls
  `Modify()` on the sequence and never touches `SetEvalDisabled`, so no asset
  edit and no autosave is introduced.
* A saved Level Sequence in a real project should be verified separately: the
  bounded fixture proves the mechanism on a transient sequence, not on a
  production asset with other bindings, spawnables and sections.
* Restoring the *pose* relies on the restored driver re-evaluating (Sequencer
  re-binding its animation instance, or the single-node animation resuming);
  the prototype restores animation state and Morph weights explicitly, but a
  component whose prior state had no driver keeps the last preview pose until
  something drives it again.
* Socket-parented targets are refused by construction. Supporting them needs a
  protocol change (send the prop relative to its attachment bone) or an explicit
  per-target "root pose is already relative" flag; neither is in the v1 profile.
* The Unreal product's single-subject contract would need: a per-session subject
  list instead of one character snapshot, one target mapping per subject id, a
  shared frame time across subjects, per-subject curve sets, and a documented
  takeover/restore rule for Sequencer and single-node animation. None of that is
  implemented in the product by this prototype.
