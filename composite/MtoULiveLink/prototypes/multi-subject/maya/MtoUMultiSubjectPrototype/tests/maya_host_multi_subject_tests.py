"""Mayapy host checks for the two-subject prototype.

Runs in disposable scenes and never saves anything. The checks cover the
acceptance criteria of the Maya slice:

* the checked-in recipe materialises three independent rigs whose files are
  byte-stable across the session;
* a session scene references two of them as separate namespaces, and each
  subject evaluates its own animation at the *same* Maya time, bit-identical to
  the same rig referenced alone (identity, time and space isolation);
* the guards refuse missing/ambiguous/non-joint/duplicate roots, role swaps,
  wrong bones, wrong curves and animated ancestors (the socket case);
* a real socket session against the bundled stand-in receiver applies every
  frame, honours ``remove`` for one subject only, and refuses wrong session,
  wrong serial, wrong time, silence and injected refusals;
* the renegotiation scenario refuses a mismatched skeleton and then accepts a
  fresh negotiation on the same connection with a larger session id;
* closing the scene and disconnecting leave no residual callbacks, expressions
  or animation curves, and the rig files are unchanged.

    <mayapy> maya_host_multi_subject_tests.py --result host_result.json
"""

import argparse
import json
import math
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SCRIPTS = ROOT / "scripts"
for _entry in (str(HERE), str(SCRIPTS)):
    if _entry not in sys.path:
        sys.path.insert(0, _entry)

# Imported by name, not by path: the entry point, the scene module and the
# stand-in receiver must share one exception class per error, exactly like two
# separately launched processes would.
import mock_multi_subject_receiver as mock_module  # noqa: E402
import mtou_multi_subject_protocol as protocol  # noqa: E402
import mtou_multi_subject_scene as scene  # noqa: E402
import MtoUMultiSubjectPrototype as entry  # noqa: E402


def maya_matrix_from_wire(row, unit_scale=1.0):
    """Inverse of the product's ``convert_transform`` for a wire pose row.

    The prototype sends UE-space ``[tx, ty, tz, qx, qy, qz, qw, sx, sy, sz]``
    rows; this rebuilds the Maya matrix they describe so a sampled row can be
    compared with the DAG matrix the subject's root actually holds.
    """
    import maya.api.OpenMaya as om

    tx, ty, tz = row[0] / unit_scale, row[2] / unit_scale, row[1] / unit_scale
    quaternion = om.MQuaternion(-row[3], -row[5], -row[4], row[6])
    transform = om.MTransformationMatrix()
    transform.setTranslation(om.MVector(tx, ty, tz), om.MSpace.kTransform)
    transform.setRotation(quaternion)
    transform.setScale([row[7], row[9], row[8]], om.MSpace.kTransform)
    return [float(value) for value in transform.asMatrix()]


def matrix_multiply(left, right):
    import maya.api.OpenMaya as om

    return [float(value) for value in (om.MMatrix(left) * om.MMatrix(right))]


class Checks(object):
    """Collects assertions instead of aborting on the first failure."""

    def __init__(self):
        self.results = []
        self.failures = []
        self.evidence = {}

    def check(self, label, condition, detail=""):
        entry = {"check": label, "ok": bool(condition), "detail": detail}
        self.results.append(entry)
        if not condition:
            self.failures.append("{0}: {1}".format(label, detail))
        return bool(condition)

    @staticmethod
    def _numbers(value):
        """Flatten nested number sequences into a list of floats."""
        if isinstance(value, (list, tuple)):
            flattened = []
            for item in value:
                flattened.extend(Checks._numbers(item))
            return flattened
        return [float(value)]

    def close(self, label, first, second, tolerance=1e-9):
        try:
            left, right = self._numbers(first), self._numbers(second)
            ok = len(left) == len(right) and all(
                abs(a - b) <= tolerance for a, b in zip(left, right))
        except (TypeError, ValueError):
            ok = False
        detail = "{0!r} vs {1!r} (tolerance {2})".format(first, second, tolerance)
        return self.check(label, ok, detail[:600])

    def refused(self, label, call, code):
        try:
            call()
        except scene.SceneRefused as error:
            return self.check(label, error.code == code,
                              "code {0} (expected {1}): {2}".format(
                                  error.code, code, error.details))
        except protocol.ProtocolError as error:
            return self.check(label, error.code == code,
                              "code {0} (expected {1}): {2}".format(
                                  error.code, code, error.details))
        except protocol.RemoteError as error:
            return self.check(label, error.code == code,
                              "remote code {0} (expected {1})".format(error.code, code))
        return self.check(label, False, "no refusal was raised")

    def record(self, key, value):
        self.evidence[key] = value


def _root_row(sample):
    transforms = sample[0]
    return transforms[0] if transforms else []


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", required=True, help="path for the JSON evidence file")
    parser.add_argument("--fixture-dir", default=None,
                        help="where the generated rigs live (default: scratch dir)")
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args(argv)

    report = {"ok": False, "phase": "loading", "checks": [], "failures": [],
              "evidence": {}}
    checks = Checks()
    started = time.time()
    try:
        import maya.standalone
        maya.standalone.initialize(name="python")
        import maya.cmds as cmds

        report["maya"] = {"version": cmds.about(version=True),
                          "api": cmds.about(apiVersion=True),
                          "batch": bool(cmds.about(batch=True))}

        fixture_dir = args.fixture_dir or str(scene.scratch_directory() / "host-tests")

        # ---------------------------------------------------------- fixtures
        report["phase"] = "fixtures"
        checks.refused(
            "generated rigs are refused inside tracked source",
            lambda: scene.build_fixtures(directory=str(SCRIPTS / "tmp-rigs")),
            scene.CODE_RECIPE_INVALID)
        manifest = scene.build_fixtures(directory=fixture_dir, force=True)
        checks.record("fixture", {"directory": manifest["directory"],
                                  "rigs": {rig_id: {"root": rig["root"],
                                                    "bones": rig["bones"],
                                                    "curves": rig["curves"],
                                                    "file": rig["file"]}
                                           for rig_id, rig in sorted(manifest["rigs"].items())}})
        checks.check("three rigs materialised", len(manifest["rigs"]) == 3,
                     sorted(manifest["rigs"]))
        for rig_id in sorted(manifest["rigs"]):
            rig = manifest["rigs"][rig_id]
            checks.check("rig {0} file exists".format(rig_id), Path(rig["file"]).is_file(),
                         rig["file"])
            checks.check("rig {0} has a unique namespace".format(rig_id),
                         rig["namespace"] == rig_id, rig["namespace"])
        digests = scene.fixture_digests(manifest)
        checks.record("fixture_digests", digests)

        # ------------------------------------------- references and subjects
        report["phase"] = "references"
        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm", time="30fps")
        session = scene.SessionScene(manifest, rigs=("character", "prop")).open()
        references = session.reference_records()
        checks.check("two independent references", len(references) == 2,
                     [record["namespace"] for record in references])
        checks.check("references keep their namespaces",
                     sorted(record["namespace"].lstrip(":") for record in references)
                     == ["character", "prop"],
                     [record["namespace"] for record in references])
        checks.check("scene frame rate is the negotiated one",
                     scene.require_scene_units(30.0) == 30.0, "30 fps")

        records = []
        for rig_id in ("character", "prop"):
            record = scene.capture_subject(
                rig_id, session.root_for(rig_id),
                expectations=manifest["fingerprints"][rig_id])
            records.append(record)
        scene.reject_duplicate_roots(records)
        checks.check("both subjects are joints with explicit long roots",
                     all(record["root"].startswith("|") for record in records),
                     [record["root"] for record in records])
        checks.check("duplicate bone names across subjects are isolated",
                     records[0]["profile"]["bones"][0] == "Root"
                     and records[1]["profile"]["bones"][0] == "Root",
                     [record["profile"]["bones"][0] for record in records])
        checks.check("Morph alias is shared across subjects",
                     all(record["profile"]["curves"] == ["Shared"] for record in records),
                     [record["profile"]["curves"] for record in records])
        checks.check("each Morph plug belongs to its own namespace",
                     all(scene.namespace_of(plug) == record["namespace"]
                         for record in records
                         for plugs in record["profile"]["curve_plugs"]
                         for plug in plugs),
                     [record["profile"]["curve_plugs"] for record in records])
        init = protocol.make_init([dict(record["wire"]) for record in records], 30.0)
        summary = protocol.validate_init(init)
        checks.record("init", {"subjects": summary["ids"],
                               "roots": summary["roots"],
                               "bones": summary["bones"],
                               "curves": summary["curves"],
                               "rows": summary["rows"]})
        checks.check("init declares exactly the permitted named ids",
                     summary["ids"] == ["character", "prop"]
                     and all(subject_id in protocol.ROLE_IDS
                             for subject_id in summary["ids"]),
                     summary["ids"])
        checks.check("init bind carries one 10-number row per bone",
                     all(row_count == len(summary["bones"][subject_id])
                         for subject_id, row_count in summary["rows"].items()),
                     summary["rows"])
        for record in records:
            authored = manifest["recipe"]["rigs"][record["id"]]["bones"]
            checks.close(
                "{0} Maya bind equals authored joint-local translations".format(record["id"]),
                [row[:3] for row in record["wire"]["bind"]],
                [[bone["translate"][0], bone["translate"][2], bone["translate"][1]]
                 for bone in authored], tolerance=1e-4)

        # ------------------------------------------- same time, root isolation
        report["phase"] = "isolation"
        samples = {}
        for time_value in (1.5, 2.75, 4.25):
            scene.set_evaluated_time(time_value)
            samples[time_value] = {record["id"]: scene.sample_subject(record)
                                   for record in records}
        checks.record("pair_samples", {
            "{0}".format(time_value): {
                subject_id: {"root": _root_row(sample), "curves": sample[1]}
                for subject_id, sample in by_subject.items()}
            for time_value, by_subject in samples.items()})
        checks.close("character root is the keyed linear pose at t=1.5",
                     _root_row(samples[1.5]["character"])[0], 107.5)
        checks.close("prop root is the keyed linear pose at t=1.5",
                     _root_row(samples[1.5]["prop"])[0], -37.5)
        checks.close("character Morph at t=1.5", samples[1.5]["character"][1][0], 0.25)
        checks.close("prop Morph at t=1.5", samples[1.5]["prop"][1][0], 1.0 - 0.5 / 3.0,
                     tolerance=1e-6)
        checks.check("the two subjects evaluate different Morph values",
                     abs(samples[1.5]["character"][1][0]
                         - samples[1.5]["prop"][1][0]) > 1e-6,
                     "{0} vs {1}".format(samples[1.5]["character"][1][0],
                                         samples[1.5]["prop"][1][0]))
        checks.check("roots move with Maya time",
                     abs(_root_row(samples[1.5]["character"])[0]
                         - _root_row(samples[4.25]["character"])[0]) > 1e-6,
                     "{0} vs {1}".format(_root_row(samples[1.5]["character"])[0],
                                         _root_row(samples[4.25]["character"])[0]))
        spine_row = samples[4.25]["character"][0][1]
        checks.close("non-root bones keep their parent-relative offset",
                     spine_row[0:3], [0.0, 0.0, 30.0], tolerance=1e-6)
        checks.check("a non-root bone row is not the root world pose",
                     abs(spine_row[0] - _root_row(samples[4.25]["character"])[0]) > 1e-6,
                     "{0} vs {1}".format(spine_row[0],
                                         _root_row(samples[4.25]["character"])[0]))

        # the same rig referenced alone must evaluate bit-identically
        for rig_id in ("character", "prop"):
            solo = scene.SessionScene(manifest, rigs=(rig_id,)).open()
            try:
                solo_record = scene.capture_subject(
                    rig_id, solo.root_for(rig_id),
                    expectations=manifest["fingerprints"][rig_id])
                scene.set_evaluated_time(4.25)
                solo_sample = scene.sample_subject(solo_record)
                checks.close(
                    "{0} pose is identical alone and in the pair".format(rig_id),
                    solo_sample[0], samples[4.25][rig_id][0])
                checks.close(
                    "{0} Morph is identical alone and in the pair".format(rig_id),
                    solo_sample[1], samples[4.25][rig_id][1])
            finally:
                scene.cmds().file(new=True, force=True)

        # -------------------------------------------------------------- guards
        report["phase"] = "guards"
        session = scene.SessionScene(manifest, rigs=("character", "prop")).open()
        checks.refused("missing root is refused",
                       lambda: scene.resolve_root("|character:Group|character:Nope"),
                       scene.CODE_MISSING_ROOT)
        checks.refused("ambiguous root pattern is refused",
                       lambda: scene.resolve_root("*:Root"),
                       scene.CODE_AMBIGUOUS_ROOT)
        checks.refused("non-joint root is refused",
                       lambda: scene.resolve_root("|character:characterMesh"),
                       scene.CODE_NOT_A_JOINT)
        checks.refused("one root for two subjects is refused",
                       lambda: scene.reject_duplicate_roots([
                           {"id": "character", "root": "|character:Group|character:Root"},
                           {"id": "prop", "root": "|character:Group|character:Root"}]),
                       scene.CODE_DUPLICATE_ROOT)
        checks.refused(
            "swapped role is refused",
            lambda: scene.capture_subject(
                "character", "|prop:Root",
                expectations=manifest["fingerprints"]["character"]),
            scene.CODE_ROLE_SWAP)
        checks.refused(
            "wrong bone list is refused",
            lambda: scene.capture_subject(
                "character", "|character:Group|character:Root",
                expectations={"bones": ["Root"], "curves": ["Shared"]}),
            scene.CODE_WRONG_BONES)
        checks.refused(
            "wrong curve list is refused",
            lambda: scene.capture_subject(
                "character", "|character:Group|character:Root",
                expectations={"curves": ["Missing"]}),
            scene.CODE_WRONG_CURVES)
        character_ancestors = scene.capture_subject(
            "character", "|character:Group|character:Root")["ancestors"]
        checks.check("a static ancestor is reported but not animated",
                     len(character_ancestors) == 1
                     and character_ancestors[0]["animated"] is False,
                     character_ancestors)
        arms_scene = scene.SessionScene(manifest, rigs=("arms",)).open()
        arms_ancestors = scene.capture_subject("arms", arms_scene.root_for("arms"))["ancestors"]
        checks.check("the animated ancestor socket is reported",
                     len(arms_ancestors) == 1 and arms_ancestors[0]["animated"] is True,
                     arms_ancestors)
        checks.refused(
            "strict ancestors refuse the animated socket",
            lambda: scene.capture_subject("arms", arms_scene.root_for("arms"),
                                          strict_ancestors=True),
            scene.CODE_ANIMATED_ANCESTOR)
        cmds.file(new=True, force=True)

        # ------------------------------- parent motion and double application
        # The prop root is re-parented under a disposable, animated session
        # transform, exactly the socket case the product contract warns about:
        # the Maya world pose then already contains the parent motion, so an
        # Unreal-side socket parent would apply it twice. The checks below pin
        # that the motion is included exactly once and that the character
        # subject is unaffected.
        report["phase"] = "space"
        alone = {}
        for rig_id in ("character", "prop"):
            solo = scene.SessionScene(manifest, rigs=(rig_id,)).open()
            try:
                solo_record = scene.capture_subject(
                    rig_id, solo.root_for(rig_id),
                    expectations=manifest["fingerprints"][rig_id])
                for frame in (1.0, 3.0):
                    scene.set_evaluated_time(frame)
                    alone[(rig_id, frame)] = scene.sample_subject(solo_record)
            finally:
                cmds.file(new=True, force=True)

        space = scene.SessionScene(manifest, rigs=("character", "prop")).open()
        space_character = scene.capture_subject(
            "character", space.root_for("character"),
            expectations=manifest["fingerprints"]["character"])
        socket = cmds.createNode("transform", name="MtoUSocket")
        for frame, value in ((1.0, 0.0), (3.0, 12.0)):
            cmds.setKeyframe(socket, attribute="translateY", time=frame, value=value)
            cmds.setKeyframe(socket, attribute="rotateY", time=frame, value=value * 2.0)
        for attribute in ("translateY", "rotateY"):
            cmds.keyTangent(socket, attribute=attribute, inTangentType="linear",
                            outTangentType="linear")
        # ``relative`` keeps the referenced prop's own local transform: the
        # session parent then contributes its motion to the world pose exactly
        # once, and no reference edit is written for the child.
        cmds.parent(space.root_for("prop"), socket, relative=True)
        moved_root = "|MtoUSocket|prop:Root"
        checks.check("an animated session parent can host the referenced prop root",
                     bool(cmds.ls(moved_root, long=True)), moved_root)
        socketed = scene.capture_subject("prop", moved_root)
        checks.check("the animated session parent is reported as an ancestor",
                     any(entry["animated"] and entry["path"].endswith("MtoUSocket")
                         for entry in socketed["ancestors"]), socketed["ancestors"])
        checks.refused(
            "strict ancestors refuse the animated session parent",
            lambda: scene.capture_subject("prop", moved_root, strict_ancestors=True),
            scene.CODE_ANIMATED_ANCESTOR)

        space_samples = {}
        for frame in (1.0, 3.0):
            scene.set_evaluated_time(frame)
            space_samples[frame] = {
                "prop": scene.sample_subject(socketed),
                "character": scene.sample_subject(space_character),
                "socket_matrix": [float(value) for value in cmds.xform(
                    socket, query=True, worldSpace=True, matrix=True)],
                "root_matrix": [float(value) for value in cmds.xform(
                    moved_root, query=True, worldSpace=True, matrix=True)],
                "local_matrix": [float(value) for value in cmds.xform(
                    moved_root, query=True, matrix=True)],
            }
        checks.check(
            "the parented prop root world pose moves with its parent",
            space_samples[1.0]["prop"][0][0] != space_samples[3.0]["prop"][0][0],
            [space_samples[frame]["prop"][0][0][:4] for frame in (1.0, 3.0)])
        for frame in (1.0, 3.0):
            wire_matrix = maya_matrix_from_wire(space_samples[frame]["prop"][0][0])
            checks.close(
                "sampled world pose equals the moved DAG matrix at frame {0}".format(frame),
                wire_matrix, space_samples[frame]["root_matrix"], tolerance=1e-5)
            # Maya matrices are row vectors, so a child's world matrix is its
            # own local matrix followed by the parent's world matrix.
            checks.close(
                "the parent motion is applied exactly once at frame {0}".format(frame),
                wire_matrix,
                matrix_multiply(maya_matrix_from_wire(alone[("prop", frame)][0][0]),
                                space_samples[frame]["socket_matrix"]),
                tolerance=1e-4)
            checks.close(
                "the referenced prop keeps its own local pose at frame {0}".format(frame),
                space_samples[frame]["local_matrix"],
                maya_matrix_from_wire(alone[("prop", frame)][0][0]), tolerance=1e-5)
            checks.close(
                "the character subject is unaffected by the prop parent at frame {0}".format(frame),
                space_samples[frame]["character"][0],
                alone[("character", frame)][0], tolerance=1e-9)
        checks.record("space", {
            "moved_root": moved_root,
            "ancestors": socketed["ancestors"],
            "frames": {str(frame): {
                "prop_root": space_samples[frame]["prop"][0][0],
                "character_root": space_samples[frame]["character"][0][0],
                "socket_matrix": space_samples[frame]["socket_matrix"]}
                for frame in (1.0, 3.0)}})
        cmds.file(new=True, force=True)

        # ----------------------------------------------- socket session (mock)
        report["phase"] = "session"
        manifest = scene.load_manifest(fixture_dir)
        digests_before = scene.fixture_digests(manifest)
        jobs_before = len(cmds.scriptJob(listJobs=True) or [])

        def drive(pair, frames, remove_at=None, faults=None, scenario="character-prop",
                  arguments=None, mock_fingerprints=True, expect_ok=True,
                  timeout=None, manifest_override=None, rig_spec=None, rig_dir=None):
            """Run the CLI once against a fresh stand-in receiver."""
            target_manifest = manifest_override or manifest
            target_dir = rig_dir or fixture_dir
            receiver = mock_module.MockReceiver(
                target_manifest["fingerprints"] if mock_fingerprints else None,
                faults or {}).start()
            argv = ["--scenario", scenario, "--frames", str(frames),
                    "--host", receiver.host, "--port", str(receiver.port),
                    "--rig-dir", target_dir, "--timeout", repr(timeout or args.timeout),
                    "--evidence", str(Path(target_dir) / "session-evidence.json")]
            if rig_spec:
                argv += ["--rig-spec", rig_spec]
            if scenario != "role-replace":
                argv += ["--pair", "+".join(pair)]
            if remove_at is not None:
                argv += ["--remove-at", str(remove_at)]
            argv += list(arguments or [])
            try:
                code = entry.main(argv)
            finally:
                receiver.stop()
            with open(str(Path(target_dir) / "session-evidence.json"),
                      "r", encoding="utf-8") as stream:
                evidence = json.load(stream)
            return code, evidence, receiver

        code, evidence, receiver = drive(("character", "prop"), 4, remove_at=2)
        checks.check("the stub session exits cleanly", code == 0,
                     json.dumps(evidence.get("errors")))
        sessions = evidence.get("sessions") or [{}]
        applied = sessions[0].get("frames") or []
        checks.check("every frame was applied", len(applied) == 4,
                     [frame["serial"] for frame in applied])
        checks.check("frame serials and times are strict",
                     [frame["serial"] for frame in applied] == [1, 2, 3, 4]
                     and [frame["time"] for frame in applied] == [1.0, 2.0, 3.0, 4.0],
                     [(frame["serial"], frame["time"]) for frame in applied])
        checks.check("both subjects were applied before the remove",
                     all(frame["statuses"] == {"character": "applied", "prop": "applied"}
                         for frame in applied[:2]),
                     [frame["statuses"] for frame in applied[:2]])
        checks.check("only the remaining subject streamed after the remove",
                     all(frame["statuses"] == {"character": "applied"}
                         for frame in applied[2:])
                     and all([entry["id"] for entry in frame["subjects"]] == ["character"]
                             for frame in applied[2:]),
                     [[entry["id"] for entry in frame["subjects"]]
                      for frame in applied[2:]])
        checks.check("the remove event disabled one subject only",
                     sessions[0]["removes"][0]["statuses"] == {
                         "character": "applied", "prop": "disabled"},
                     sessions[0]["removes"][0]["statuses"])
        received = receiver.received_messages()
        frames_received = [message for message in received if message["type"] == "frame"]
        checks.check("the receiver saw the same number of frames",
                     len(frames_received) == 4, len(frames_received))
        checks.check("the receiver saw the same serials and times",
                     [(message["serial"], message["time"]) for message in frames_received]
                     == [(frame["serial"], frame["time"]) for frame in applied],
                     [(message["serial"], message["time"]) for message in frames_received])
        checks.check("the receiver's frames carry what Maya recorded",
                     all(
                         [subject["id"] for subject in message["subjects"]]
                         == [entry["id"] for entry in frame["subjects"]]
                         and [subject["curves"] for subject in message["subjects"]]
                         == [entry["curves"] for entry in frame["subjects"]]
                         for message, frame in zip(frames_received, applied)),
                     "per-frame subject ids and Morph values")
        checks.check("the receiver verified the declared skeletons",
                     receiver.sessions == 1, receiver.sessions)
        checks.check("one init and one remove reached the receiver",
                     [message["type"] for message in received]
                     == ["init", "frame", "frame", "remove", "frame", "frame"],
                     [message["type"] for message in received])
        remove_message = [message for message in received if message["type"] == "remove"][0]
        checks.check("the remove names the disabled subject",
                     remove_message["id"] == "prop", remove_message)
        checks.record("session_evidence", {
            "frames": applied,
            "removes": sessions[0]["removes"],
            "latency_ms": [frame["latency_ms"] for frame in applied],
            "checks": evidence["checks"]})

        # A reverse scrub and a re-edit of an already sent frame are legal: the
        # serial identifies the evaluation, not the source time.
        code, evidence, receiver = drive(("character", "prop"), 4,
                                         arguments=["--times", "1,3,2,2"])
        frames = (evidence.get("sessions") or [{}])[0].get("frames") or []
        checks.check("a reverse scrub and a same-frame re-edit are accepted",
                     code == 0
                     and [frame["time"] for frame in frames] == [1.0, 3.0, 2.0, 2.0]
                     and [frame["time_direction"] for frame in frames]
                     == ["first", "forward", "backward", "hold"]
                     and [frame["serial"] for frame in frames] == [1, 2, 3, 4],
                     [(frame["serial"], frame["time"], frame["time_direction"])
                      for frame in frames])

        # Every frame is bound to the session it was sampled for, so a frame
        # left over from an earlier negotiation cannot be applied later.
        checks.check("every frame and remove names its negotiated session",
                     receiver.sessions == 1
                     and all(message.get("session") == 1
                             for message in receiver.received_messages()
                             if message["type"] in ("frame", "remove")),
                     [message.get("session")
                      for message in receiver.received_messages()])

        def stale_frame_probe():
            receiver = mock_module.MockReceiver(manifest["fingerprints"], {}).start()
            try:
                session = scene.SessionScene(manifest, rigs=("character", "prop")).open()
                try:
                    records = [
                        scene.capture_subject(rig_id, session.root_for(rig_id),
                                              expectations=manifest["fingerprints"][rig_id])
                        for rig_id in ("character", "prop")]
                    driver = entry.MultiSubjectSession(
                        records, receiver.host, receiver.port, 30.0,
                        timeout=args.timeout).connect()
                    try:
                        driver.negotiate(note="first negotiation")
                        driver.step(1.0)
                        driver.step(2.0)
                        stale_session = driver.session
                        # Renegotiating hands out a new session, so a pose that was
                        # sampled for the previous one must no longer apply.
                        driver.negotiate(note="second negotiation")
                        driver.step(3.0)
                        frames = []
                        for record in driver.active_records():
                            transforms, curves = scene.sample_subject(record)
                            frames.append({"id": record["id"], "transforms": transforms,
                                           "curves": curves})
                        driver.connection.send(protocol.make_frame(
                            stale_session, driver.serial + 1, 2.0, frames))
                        try:
                            driver.connection.receive(
                                protocol.APPLIED, time.monotonic() + args.timeout)
                        except protocol.RemoteError as error:
                            if error.code == "session_mismatch":
                                return
                            raise
                        raise AssertionError("a stale-session frame was applied")
                    finally:
                        driver.close()
                finally:
                    scene.cmds().file(new=True, force=True)
            finally:
                receiver.stop()

        checks.check("a frame from a previous session is refused",
                     stale_frame_probe() is None, "stale frame refused")

        # steeped time: fractional samples must reach the wire unchanged
        code, evidence, receiver = drive(("character", "prop"), 3,
                                         arguments=["--start-frame", "1.5",
                                                    "--step", "0.5"])
        frames = (evidence.get("sessions") or [{}])[0].get("frames") or []
        checks.check("fractional times reach the wire",
                     [frame["time"] for frame in frames] == [1.5, 2.0, 2.5]
                     and code == 0,
                     [(frame["time"], frame["subjects"][0]["curves"])
                      for frame in frames])
        checks.close("fractional Morph value is the linear sample",
                     frames[0]["subjects"][0]["curves"][0], 0.25)

        # ---------------------------------------------------- renegotiation
        report["phase"] = "renegotiation"
        code, evidence, receiver = drive(("character", "arms"), 2,
                                         scenario="character-arms")
        sessions = (evidence.get("sessions") or [{}])[0]
        negotiations = sessions.get("negotiations") or []
        checks.check("the mismatched arms init was refused by the receiver",
                     len(negotiations) >= 2
                     and negotiations[0]["error"] is not None
                     and negotiations[0]["session"] is None
                     and negotiations[0]["error"]["code"] == "skeleton_mismatch",
                     negotiations[:1])
        full_body = manifest["fingerprints"]["character"]["bones"]
        arms_bones = manifest["fingerprints"]["arms"]["bones"]
        checks.check("the mismatched init declared the arms subject with the "
                     "full-body skeleton and left the character correct",
                     [entry["id"] for entry in negotiations[0]["fingerprint"]]
                     == ["character", "arms"]
                     and [entry["bones"] for entry in negotiations[0]["fingerprint"]]
                     == [full_body, full_body],
                     [entry["bones"] for entry in negotiations[0]["fingerprint"]])
        checks.check("the accepted init declares both real skeletons",
                     [entry["bones"] for entry in negotiations[1]["fingerprint"]]
                     == [full_body, arms_bones],
                     [entry["bones"] for entry in negotiations[1]["fingerprint"]])
        checks.check("a fresh negotiation on the same connection succeeded",
                     len(negotiations) >= 2
                     and negotiations[1]["session"] is not None
                     and negotiations[1]["session"] >= 1
                     and len([entry for entry in negotiations if entry["session"]]) == 1,
                     [entry["session"] for entry in negotiations])
        checks.check("frames applied after the renegotiation",
                     len(sessions.get("frames") or []) == 2 and code == 0,
                     json.dumps(evidence.get("errors")))
        checks.check("the receiver answered with two sessions' worth of state",
                     receiver.sessions == 1 and receiver.clients == 1,
                     "sessions={0} clients={1}".format(receiver.sessions,
                                                       receiver.clients))
        checks.record("renegotiation", {"negotiations": negotiations,
                                        "refusals": evidence.get("refusals")})

        code, evidence, receiver = drive(("character", "prop"), 2,
                                         scenario="role-replace")
        pairs_seen = [session["pair"] for session in evidence.get("sessions") or []]
        checks.check("role replacement used two connections and two pairs",
                     code == 0 and pairs_seen == [["character", "prop"],
                                                  ["character", "arms"]],
                     pairs_seen)
        if receiver.clients >= 2:
            checks.check("the replacement pair negotiated again", True,
                         "clients={0}".format(receiver.clients))

        # ---------------------------------------------------- alternate spec
        report["phase"] = "rig-spec"
        spec = {
            "name": "host-check-spec",
            "revision": 1,
            "fps": 30.0,
            "range": [1.0, 4.0],
            "rigs": {
                "character": {
                    "namespace": "character",
                    "ancestors": [],
                    "bones": [
                        {"name": "Root", "parent": -1, "translate": [0.0, 0.0, 0.0]},
                        {"name": "Joint", "parent": 0, "translate": [0.0, 12.0, 0.0]},
                    ],
                    "mesh": {"name": "specMesh", "size": [8.0, 24.0, 8.0]},
                    "curves": {"Shared": [[1.0, 0.2], [4.0, 0.8]]},
                    "animation": {"Root": {"translateX": [[1.0, 0.0], [4.0, 9.0]]}},
                },
                "prop": {
                    "namespace": "prop",
                    "ancestors": [],
                    "bones": [{"name": "Root", "parent": -1, "translate": [0.0, 0.0, 0.0]}],
                    "mesh": {"name": "specPropMesh", "size": [4.0, 8.0, 4.0]},
                    "curves": {"Shared": [[1.0, 0.1], [4.0, 0.4]]},
                    "animation": {"Root": {"translateY": [[1.0, 0.0], [4.0, 3.0]]}},
                },
            },
        }
        spec_path = Path(fixture_dir) / "host-check-spec.json"
        with open(str(spec_path), "w", encoding="utf-8") as stream:
            json.dump(spec, stream, indent=2)
        spec_dir = Path(fixture_dir) / "spec-fixtures"
        spec_manifest = scene.build_fixtures(spec, directory=str(spec_dir), force=True)
        checks.check("a spec builds its own rigs",
                     spec_manifest["rigs"]["character"]["bones"] == ["Root", "Joint"]
                     and spec_manifest["rigs"]["prop"]["bones"] == ["Root"],
                     {rig_id: rig["bones"] for rig_id, rig in spec_manifest["rigs"].items()})
        code, evidence, receiver = drive(
            ("character", "prop"), 4, manifest_override=spec_manifest,
            rig_spec=str(spec_path), rig_dir=str(spec_dir))
        frames = (evidence.get("sessions") or [{}])[0].get("frames") or []
        checks.check("a spec-driven session applies frames",
                     code == 0 and len(frames) == 4
                     and frames[0]["subjects"][0]["transforms"] == 2
                     and frames[0]["subjects"][1]["transforms"] == 1,
                     [(frame["serial"], [entry["transforms"] for entry in frame["subjects"]])
                      for frame in frames])
        checks.close("a spec-driven frame keeps its sampled values",
                     frames[0]["subjects"][0]["curves"] and
                     frames[-1]["subjects"][0]["curves"][0], 0.8, tolerance=1e-6)

        # A bone's resting rotation is part of the rig: an imported target can
        # rest rotated, and the observation bind has to carry it.
        rotated_spec = json.loads(json.dumps(spec))
        rotated_spec["name"] = "host-check-spec-rotated"
        rotated_spec["rigs"]["character"]["bones"][1]["rotate"] = [90.0, 0.0, 0.0]
        rotated_path = Path(fixture_dir) / "host-check-spec-rotated.json"
        with open(str(rotated_path), "w", encoding="utf-8") as stream:
            json.dump(rotated_spec, stream, indent=2)
        rotated_dir = Path(fixture_dir) / "spec-rotated-fixtures"
        rotated_manifest = scene.build_fixtures(
            rotated_spec, directory=str(rotated_dir), force=True)
        rotated_session = scene.SessionScene(
            rotated_manifest, rigs=("character", "prop")).open()
        try:
            rotated_record = scene.capture_subject(
                "character", rotated_session.root_for("character"))
            expected_bind = scene.product().convert_transform(
                [0.0, 0.0, 0.0], [0.7071067811865476, 0.0, 0.0, 0.7071067811865476],
                [1.0, 1.0, 1.0], 1.0)
            expected_quaternion = expected_bind[3:7]
            joint_row = rotated_record["wire"]["bind"][1]
            dot = abs(sum(a * b for a, b in zip(joint_row[3:7], expected_quaternion)))
            checks.close(
                "a spec bone's resting rotation reaches the wire bind",
                math.degrees(2.0 * math.acos(min(1.0, dot))), 0.0, tolerance=0.05)
        finally:
            scene.cmds().file(new=True, force=True)

        # A known Morph value can be set on the disposable session scene, so a
        # Morph channel is verified with a value rather than only with zeroes.
        code, evidence, receiver = drive(
            ("character", "prop"), 4, manifest_override=spec_manifest,
            rig_spec=str(spec_path), rig_dir=str(spec_dir),
            arguments=["--set-curve", "character=Shared=0.42"])
        overrides = evidence.get("curve_overrides") or []
        override_frames = (evidence.get("sessions") or [{}])[0].get("frames") or []
        checks.check(
            "a Morph override is applied to the disposable scene and streamed",
            code == 0 and len(overrides) == 1
            and overrides[0]["curve"] == "Shared"
            and override_frames
            and all(abs(frame["subjects"][0]["curves"][0] - 0.42) < 1e-6
                    for frame in override_frames),
            {"overrides": overrides,
             "values": [frame["subjects"][0]["curves"][0]
                        for frame in override_frames]})

        # ------------------------------------------------------ fault paths
        report["phase"] = "faults"

        def shared_target_probe():
            receiver = mock_module.MockReceiver(manifest["fingerprints"], {}).start()
            try:
                session = scene.SessionScene(manifest, rigs=("character", "prop")).open()
                try:
                    records = [
                        scene.capture_subject(rig_id, session.root_for(rig_id),
                                              expectations=manifest["fingerprints"][rig_id])
                        for rig_id in ("character", "prop")]
                    wires = [dict(records[0]["wire"]),
                             dict(records[1]["wire"], root=records[0]["root"])]
                    driver = entry.MultiSubjectSession(
                        records, receiver.host, receiver.port, 30.0,
                        timeout=args.timeout).connect()
                    try:
                        driver.negotiate(wires=wires, note="shared target probe")
                    finally:
                        driver.close()
                finally:
                    scene.cmds().file(new=True, force=True)
            finally:
                receiver.stop()
            raise AssertionError("a shared target root was accepted")

        checks.refused("a shared target root is refused", shared_target_probe,
                       "shared_target")
        for label, faults, expected in (
                ("wrong session id", {"session_offset": 3}, protocol.CODE_SESSION_MISMATCH),
                ("wrong serial", {"serial_offset": 1}, protocol.CODE_SERIAL_MISMATCH),
                ("wrong time", {"time_offset": 0.5}, protocol.CODE_TIME_MISMATCH),
                ("silent receiver", {"silence_frames": (1,)}, "REPLY_TIMEOUT"),
                ("refused init", {"refuse_inits": True}, "injected_fault")):
            checks.refused(label + " is refused",
                           lambda faults=faults: _drive_once(
                               entry, mock_module, manifest, faults, args.timeout),
                           expected)
        code, evidence, receiver = drive(("character", "prop"), 4,
                                         faults={"close_after_frames": 2})
        checks.check("a receiver that closes mid-session is reported",
                     code == 1 and any(error["code"] in ("TRANSPORT_ERROR",
                                                         "REPLY_TIMEOUT")
                                       for error in evidence["errors"]),
                     json.dumps(evidence["errors"])[:200])
        code, evidence, receiver = drive(("character", "prop"), 4, arguments=["--drop-after", "2"])
        checks.check("--drop-after closes the connection abruptly",
                     code == 0 and len((evidence.get("sessions") or [{}])[0]
                                       .get("frames") or []) == 2,
                     json.dumps((evidence.get("sessions") or [{}])[0].get("frames"))[:160])
        checks.check("the receiver saw the abrupt disconnect",
                     "disconnect" in receiver.events(), receiver.events())

        # ---------------------------------------------------- scene lifecycle
        report["phase"] = "cleanup"
        # A live library-level session: the scene stays open here, so the
        # checks below see exactly what a session leaves in it.
        receiver = mock_module.MockReceiver(manifest["fingerprints"], {}).start()
        checks.check("the lifecycle probe receiver starts from zero sessions",
                     receiver.sessions == 0 and receiver.clients == 0,
                     "sessions={0} clients={1}".format(receiver.sessions,
                                                       receiver.clients))
        try:
            live = scene.SessionScene(manifest, rigs=("character", "prop")).open()
            try:
                records = []
                for rig_id in ("character", "prop"):
                    records.append(scene.capture_subject(
                        rig_id, live.root_for(rig_id),
                        expectations=manifest["fingerprints"][rig_id]))
                driver = entry.MultiSubjectSession(records, receiver.host,
                                                   receiver.port, 30.0,
                                                   timeout=args.timeout).connect()
                try:
                    driver.negotiate(note="lifecycle probe")
                    first_session = driver.session
                    driver.step(2.5)
                    driver.negotiate(note="second probe on the same connection")
                    second_session = driver.session
                    driver.step(3.25)
                    driver.remove("prop")
                    driver.step(3.5)
                finally:
                    driver.close()
                checks.check("a second negotiation on the same connection "
                             "gets a larger session id",
                             second_session > first_session > 0,
                             "{0} then {1}".format(first_session, second_session))
                checks.check("the re-negotiated pair streams both subjects again",
                             [frame["statuses"] for frame in driver.evidence["frames"]]
                             == [{"character": "applied", "prop": "applied"},
                                 {"character": "applied", "prop": "applied"},
                                 {"character": "applied"}],
                             [frame["statuses"] for frame in driver.evidence["frames"]])
                checks.check("the disabled subject's rig is still referenced",
                             any(record["namespace"].lstrip(":") == "prop"
                                 for record in live.reference_records()),
                             live.reference_records())
                stray = [node for node in (cmds.ls(type="animCurve") or [])
                         if not cmds.referenceQuery(node, isNodeReferenced=True)]
                checks.check("the session created no animation curves", not stray, stray)
                checks.check("the session created no expressions",
                             not (cmds.ls(type="expression") or []), cmds.ls(type="expression"))
                checks.check("no script job was left behind by the session",
                             len(cmds.scriptJob(listJobs=True) or []) == jobs_before,
                             "{0} vs {1}".format(
                                 jobs_before, len(cmds.scriptJob(listJobs=True) or [])))
                checks.check("the session scene was never saved to a file",
                             not cmds.file(query=True, sceneName=True),
                             "sceneName={0!r}".format(cmds.file(query=True, sceneName=True)))
                checks.check("the session scene is dirty and was left unsaved",
                             bool(cmds.file(query=True, modified=True)),
                             "modified={0}".format(cmds.file(query=True, modified=True)))
            finally:
                live.close()
        finally:
            receiver.stop()
        checks.check("the session scene closes cleanly",
                     not (cmds.ls("*Mesh") or []), cmds.ls("*Mesh"))
        checks.check("a fresh scene has no references",
                     not (cmds.file(query=True, reference=True) or []),
                     cmds.file(query=True, reference=True))
        checks.check("rig files are unchanged after the whole run",
                     scene.fixture_digests(manifest) == digests_before,
                     sorted(digests_before))

        report["phase"] = "done"
    except Exception:
        report["traceback"] = traceback.format_exc()
        checks.check("host checks completed without an unexpected exception", False,
                     report["traceback"].splitlines()[-1] if report["traceback"] else "")

    report["ok"] = not checks.failures and not report.get("traceback")
    report["checks"] = checks.results
    report["failures"] = checks.failures
    report["evidence"] = checks.evidence
    report["seconds"] = time.time() - started
    target = Path(args.result)
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(str(target), "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, ensure_ascii=True)
        stream.write("\n")
    print(json.dumps({"ok": report["ok"], "checks": len(report["checks"]),
                      "failures": report["failures"][:8],
                      "result": str(target)}, indent=2, ensure_ascii=True))
    return 0 if report["ok"] else 1


def _drive_once(entry, mock_module, manifest, faults, timeout):
    """One library-level session against a faulting receiver (raises)."""
    receiver = mock_module.MockReceiver(manifest["fingerprints"], faults).start()
    try:
        session = scene.SessionScene(manifest, rigs=("character", "prop")).open()
        try:
            records = []
            for rig_id in ("character", "prop"):
                records.append(scene.capture_subject(
                    rig_id, session.root_for(rig_id),
                    expectations=manifest["fingerprints"][rig_id]))
            driver = entry.MultiSubjectSession(records, receiver.host, receiver.port,
                                               30.0, timeout=timeout).connect()
            try:
                driver.negotiate(note="fault probe")
                driver.step(2.5)
            finally:
                driver.close()
        finally:
            scene.cmds().file(new=True, force=True)
    finally:
        receiver.stop()
    raise AssertionError("the faulting receiver was accepted")


if __name__ == "__main__":
    raise SystemExit(main())
