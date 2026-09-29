# MtoU multi-subject prototype: Maya side

Maya half of the bounded two-subject realtime prototype for Issue #54. It
drives **two independently referenced bound skeletons** against one Unreal
receiver: one Maya evaluated time and one strictly increasing serial (starting
at 1, as the receiver requires) per atomic subject pair, with the Unreal
`applied` echo recorded before the next frame.

This prototype's contract is version 1: newline-delimited compact JSON over
TCP, 1 MiB per line, at most two subjects with ids from
`character`/`prop`/`arms`; `../protocol.md` records the shared host contract.
Its parent product contract is the v9 protocol of
`composite/MtoULiveLink/README.md`; the product's v9 protocol, the product Maya
entry point and cached playback are untouched by this prototype.

## What it proves

* two Maya references evaluate **two independent skeletal poses at the same
  time**, each identical to the same rig referenced alone, for both registered
  pairs (`character`+`prop` and `character`+`arms`);
* duplicate bone names (`Root` on both subjects) and a duplicate Morph alias
  (`Shared`) do not cross-talk: every Morph plug is checked to belong to its own
  subject namespace;
* identity, time and space guards: explicit long root strings, refused
  missing/ambiguous/non-joint roots, refused role swap, wrong bones, wrong
  curves and animated ancestors (the socket case), duplicate roots, stale
  session/serial/time echoes and silent receivers;
* a referenced prop root re-parented under an animated session transform still
  samples a **world** pose that includes that parent's motion exactly once
  (checked against the DAG matrix and against the parent-composed local pose),
  while the other subject keeps its own pose;
* `remove` disables one subject only, the other keeps streaming, and the
  disabled subject's rig stays intact;
* closing the scene leaves no callbacks, expressions or animation curves, and
  the generated rig files are byte-identical before and after a session;
* role replacement is a **new negotiation**: the replacement scenario declares
  the `arms` subject with the full-body skeleton (the receiver's target check
  refuses it and applies no pose) while `character` stays correct, a fresh init
  on the same connection gets a session id, and swapping the pair onto a
  different skeleton uses a new connection.

## Space and parent motion

Each subject's first transform row is the *world* pose of its Maya root, so
motion from a reference parent (a socket or group above the root) is already
included exactly once; non-root rows stay parent-relative inside their own
subject. The positive route is one **world-anchored** Unreal Actor per subject,
which applies that row once. Attaching the Unreal target to a moving socket as
well would apply the parent motion twice, so Maya reports every animated
ancestor in `subjects[].ancestors` and refuses it under `--strict-ancestors`
(`ANIMATED_ANCESTOR`), and the Unreal receiver refuses an anchor that carries
the motion itself (its `anchor_conflict`). The host checks pin the single
application with a disposable animated session parent: the moved root's world
pose equals `local × parent`, the referenced prop keeps its own local pose, and
the other subject is unaffected.

Scope limits (by design): no cached playback, no Additional Parts, no
auto-retarget, no socket-parent motion on the Unreal side, and no host versions
beyond the ones exercised below.

## Files

| Path | Contents |
| --- | --- |
| `scripts/mtou_multi_subject_protocol.py` | Pure wire contract: framing, builders, strict validation, reply/session rules, evidence helpers. No Maya imports. |
| `scripts/mtou_multi_subject_scene.py` | Fixture recipe materialisation, reference session scenes, subject capture through the product's `_capture_subject`, sampling through the product's `_sample_pose`/`convert_transform`, and the identity/time/space guards. |
| `scripts/mtou_multi_subject_recipe.json` | The checked-in fixture recipe (three rigs, keyframes, Morph timelines). `--rig-spec FILE` builds a different recipe of the same shape, for matching another host's target skeletons. |
| `scripts/MtoUMultiSubjectPrototype.py` | CLI and session driver: `--write-fixtures`, `--probe`, scenarios `character-prop`, `character-arms`, `role-replace`. |
| `tests/maya_host_multi_subject_tests.py` | mayapy host checks: fixtures, references, same-time isolation, guards, socket session, renegotiation, fault paths, cleanup. |
| `tests/maya_peer.py` | Standalone peer for a real Unreal receiver; `--spawn-mock` runs the stand-in receiver in-process. |
| `tests/mock_multi_subject_receiver.py` | Stand-in Unreal receiver: target-skeleton verification, `ready`/`applied`/`error`, `remove`, disconnect clearing, fault injection. |
| `tests/test_multi_subject_protocol.py` | Pure unit tests for the wire contract; runs under CPython and mayapy. |

Generated `*.ma` rigs are **never** written into tracked source: the recipe is
checked in and materialised under `<repo>/../.tmp/mtou-issue54/multi-subject`
(override with `--rig-dir` or `MTOU_MULTI_SUBJECT_SCRATCH`). A fixture directory
inside the repository is refused unless it is under a `.tmp` path. A `--rig-spec`
recipe gets its own subdirectory (`<scratch>/<recipe-name>`), so switching
recipes can never reuse another recipe's rig files.

The default recipe is the shared integration baseline: the Unreal fixture
builds the same bone sets and the same `Shared` Morph curve per subject, so an
init from this sender matches its targets without any override. The explicit
paths are `|character:Group|character:Root`, `|prop:Root` and
`|arms:Socket|arms:ArmsRoot`; the final segment is each subject's first bone
(`Root`, `Root` and `ArmsRoot` respectively). `bind` carries one ten-number row
per declared bone.

## Running

`mayapy` is Maya's standalone interpreter; it is not on `PATH` by default.

```bash
# 1. pure contract tests (any CPython 3)
python -m unittest discover \
  -s composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype/tests \
  -t composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype

# 2. mayapy host checks (disposable scenes, JSON evidence)
"$MAYAPY" composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype/tests/maya_host_multi_subject_tests.py --result host_result.json

# 3. self-contained session: the peer starts the stand-in receiver itself
"$MAYAPY" composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype/tests/maya_peer.py --spawn-mock --scenario character-prop \
  --frames 4 --remove-at 2 --evidence peer_evidence.json

# 4. read-only inventory of the generated pair (no socket, no save)
"$MAYAPY" composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype/scripts/MtoUMultiSubjectPrototype.py --probe \
  --evidence probe.json

# 5. inspect a supplied real scene read-only, paired with a generated prop
"$MAYAPY" composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype/scripts/MtoUMultiSubjectPrototype.py --probe \
  --scene /path/to/real.ma --reference-rig prop \
  --subject "character=|Group|root" --probe-times 1,1.5 --evidence probe.json
```

`--probe` opens the supplied scene, reads units, references, root hierarchies,
Morph names and sampled poses, and never saves; `--fps` is only compared when
given, because a probe reports the scene's own frame rate.

## Driving a real Unreal receiver

The Unreal adapter is a separate component. Its receiver is expected to listen
on TCP and answer `ready`/`applied`/`error`; the peer is the Maya side of that
conversation:

```bash
"$MAYAPY" composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype/tests/maya_peer.py \
  --host 127.0.0.1 --port 54340 \
  --scenario character-prop --frames <N> --fps 30 --start-frame 1 \
  --evidence peer_evidence.json [--remove-at <frame>] [--drop-after <frames>]
```

* `--scenario character-prop` — the mandatory pair: init, `ready`, one
  `applied` per frame; `--remove-at N` disables the second subject after frame
  `N` and the remaining frames carry one subject.
* `--scenario character-arms` — the replacement target, pair `character` +
  `arms` (the ids the Unreal fixture registers): the first init declares the
  `arms` subject with the full-body skeleton and must be refused
  (`skeleton_mismatch`, no session, no pose); a fresh init on the **same
  connection** must then be accepted with a new session id, and frames apply
  afterwards.
* `--scenario role-replace` — `character`+`prop`, then `character`+`arms` on a
  **new connection** (new negotiation, no hot swap). This sequence is exercised
  against the bundled stand-in receiver; a real Unreal receiver serves one
  scenario per listener, so a real cross-host run restarts (or starts a second)
  listener for the second pair instead of remapping targets in place.
* `--drop-after N` closes the socket abruptly after `N` frames to prove the
  receiver clears its state on disconnect.

Exit code 0 means the scenario's expected outcome happened; the evidence JSON is
written either way.

### Evidence JSON

```json
{
  "ok": true,
  "tool": "MtoUMultiSubjectPrototype",
  "protocol": {"name": "MtoUMultiSubject", "version": 1},
  "maya": {"version": "2024", "api": 20240200, "batch": true},
  "requested_fps": 30.0,
  "fixture": {"directory": "...", "range": [1.0, 6.0], "fps": 30.0,
              "rigs": {"character": {"file": "...", "root": "...", "bones": []}}},
  "scenes": [{"pair": ["character", "prop"], "fps": 30.0, "linear_unit": "cm",
              "opened_clean": true, "modified_before_close": true,
              "references": [{"node": "characterRN", "namespace": ":character",
                              "file": "..."}]}],
  "sessions": [{
    "pair": ["character", "prop"],
    "session": 1,
    "negotiations": [{"note": "negotiated init", "session": 1, "error": null,
                      "fingerprint": [{"id": "character", "root": "...",
                                       "bones": [], "curves": []}]}],
    "subjects": [{"id": "character", "root": "|character:Group|character:Root",
                  "namespace": "character", "bones": [], "parents": [],
                  "curves": ["Shared"], "curve_plugs": [["character:..."]]}],
    "frames": [{"serial": 1, "time": 1.0, "latency_ms": 0.31,
                "statuses": {"character": "applied", "prop": "applied"},
                "subjects": [{"id": "character", "root": [10 numbers],
                              "curves": [0.5], "transforms": 4,
                              "digest": "sha1 of the transform rows"}]}],
    "removes": [{"id": "prop", "statuses": {"character": "applied",
                                            "prop": "disabled"},
                 "remaining": ["character"], "serial": 1, "time": 2.0}]
  }],
  "checks": [{"check": "...", "ok": true, "detail": "..."}],
  "errors": [],
  "cleanup": {"scene_saved": false, "session_scene_closed": true}
}
```

`frames[]` records exactly what Maya sent (root world pose, Morph values and a
digest of every transform row) plus the round-trip latency, so an Unreal-side
record of received/applied values can be cross-read against it.

## Compatibility

Exercised with Maya 2022 (`apiVersion` 20220400) and Maya 2024 (`apiVersion`
20240200) in `mayapy` batch mode. The Maya session scene must be centimetres at
the negotiated frame rate; the prototype refuses anything else instead of
converting. Root poses use the product's `convert_transform` axes and its
inclusive-world root convention; non-root bones are parent-relative inside their
own subject.
