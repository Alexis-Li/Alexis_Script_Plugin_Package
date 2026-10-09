"""Maya side of the bounded two-subject realtime prototype.

One Maya session drives at most two independently referenced bound skeletons
against one Unreal receiver over the prototype's newline JSON protocol (see
``mtou_multi_subject_protocol.py``). The session samples *one* Maya evaluated
time per serial for the atomic subject pair, waits for the receiver's
``applied`` echo, and records the values it actually sent plus the latency.

Modes (no mode ever saves a scene):

* ``--write-fixtures``: materialise the checked-in recipe into a scratch
  directory outside the repository;
* ``--probe``: read-only inventory and sampling of the pair (also of a supplied
  real scene through ``--scene``);
* default: run a scenario against ``--host``/``--port`` and write JSON
  evidence for the Unreal automation test.

Scenarios: ``character-prop`` (the mandatory pair), ``character-arms`` (a
deliberately mismatched init refused, then a fresh negotiation on the same
connection) and ``role-replace`` (character+prop, then arms+prop on a new
connection).

    mayapy MtoUMultiSubjectPrototype.py --scenario character-prop \
        --host 127.0.0.1 --port 54340 --frames 4 --evidence session.json
"""

import argparse
import hashlib
import json
import socket
import sys
import time
import traceback
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import mtou_multi_subject_protocol as protocol  # noqa: E402
import mtou_multi_subject_scene as scene  # noqa: E402

TOOL_NAME = "MtoUMultiSubjectPrototype"
DEFAULT_TIMEOUT = 10.0
SCENARIOS = ("character-prop", "character-arms", "role-replace")
CODE_REPLY_TIMEOUT = "REPLY_TIMEOUT"
CODE_TRANSPORT = "TRANSPORT_ERROR"
CODE_SCENARIO_EXPECTATION = "SCENARIO_EXPECTATION"
CODE_REFUSED = "SCENE_REFUSED"


def digest_transforms(transforms):
    payload = json.dumps(transforms, ensure_ascii=True, allow_nan=False,
                         separators=(",", ":")).encode("utf-8")
    return hashlib.sha1(payload).hexdigest()


class Connection(object):
    """One TCP connection with bounded newline framing and deadline reads."""

    def __init__(self, host, port, timeout=DEFAULT_TIMEOUT, connect=None):
        self.host = host
        self.port = int(port)
        self.timeout = float(timeout)
        self._connect = connect or socket.create_connection
        self._socket = None
        self._framer = protocol.LineFramer()

    def connect(self):
        self._socket = self._connect((self.host, self.port), self.timeout)
        self._socket.settimeout(self.timeout)
        return self

    def send(self, message):
        if self._socket is None:
            raise protocol.ProtocolError(CODE_TRANSPORT, "connection is not open")
        self._socket.sendall(protocol.encode_line(message))

    def receive(self, expected_type, deadline):
        """Next reply of the expected type, or ``RemoteError`` for an error reply."""
        while True:
            for line in self._framer.take_lines():
                reply = protocol.parse_message(line)
                protocol.validate_reply_type(reply, expected_type)
                return reply
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                raise protocol.ProtocolError(
                    CODE_REPLY_TIMEOUT, "receiver did not answer in time",
                    "waiting for {0}".format(expected_type))
            self._socket.settimeout(remaining)
            try:
                data = self._socket.recv(65536)
            except socket.timeout:
                raise protocol.ProtocolError(
                    CODE_REPLY_TIMEOUT, "receiver did not answer in time",
                    "waiting for {0}".format(expected_type))
            except OSError as error:
                raise protocol.ProtocolError(
                    CODE_TRANSPORT, "connection failed while reading",
                    "{0}: {1}".format(type(error).__name__, error))
            if not data:
                raise protocol.ProtocolError(
                    CODE_TRANSPORT, "receiver closed the connection",
                    "waiting for {0}".format(expected_type))
            self._framer.feed(data)

    def close(self):
        if self._socket is None:
            return
        try:
            self._socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._socket.close()
        self._socket = None

    def drop(self):
        """Close abruptly (RST), the way a crashed peer disappears."""
        if self._socket is None:
            return
        try:
            self._socket.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER,
                b"\x01\x00\x00\x00\x00\x00\x00\x00")
        except OSError:
            pass
        self._socket.close()
        self._socket = None


class MultiSubjectSession(object):
    """Drives one connection and records everything it sent and heard."""

    def __init__(self, records, host, port, fps, timeout=DEFAULT_TIMEOUT):
        self.records = list(records)
        self.host = host
        self.port = int(port)
        self.fps = float(fps)
        self.timeout = float(timeout)
        self.connection = Connection(host, port, timeout)
        self.session = None
        self.summary = None
        #: Applied Morph overrides, re-applied for every sampled frame.
        self.curve_overrides = []
        #: Frames applied on this connection; the next serial is ``serial + 1``
        #: and the first frame is serial 1, which is what the receiver requires.
        self.serial = 0
        self.last_time = None
        self.evidence = {
            "pair": [record["id"] for record in self.records],
            "session": None,
            "negotiations": [],
            "subjects": [],
            "frames": [],
            "removes": [],
        }

    # ------------------------------------------------------------- transport

    def connect(self):
        self.connection.connect()
        return self

    def _send(self, message):
        self.connection.send(message)
        return message

    def _reply(self, expected_type):
        return self.connection.receive(expected_type,
                                       time.monotonic() + self.timeout)

    # ----------------------------------------------------------- negotiation

    def _wire_subjects(self, wires=None):
        if wires is None:
            return [dict(record["wire"]) for record in self.records]
        overridden = {wire["id"]: wire for wire in wires}
        return [overridden.get(record["id"], record["wire"])
                for record in self.records]

    def negotiate(self, wires=None, note=None):
        """Send one init and wait for ``ready``.

        A connection may negotiate again after a semantic error reply or after
        a completed session: this is a new negotiation and the receiver must
        hand out a new, larger session id.
        """
        subjects = self._wire_subjects(wires)
        init = protocol.make_init(subjects, self.fps)
        entry = {
            "note": note,
            "subjects": [subject["id"] for subject in subjects],
            "fingerprint": [{"id": subject["id"], "root": subject["root"],
                             "bones": [bone["name"] for bone in subject["bones"]],
                             "curves": list(subject["curves"])}
                            for subject in subjects],
            "session": None,
            "error": None,
        }
        self._send(init)
        try:
            reply = self._reply(protocol.READY)
        except protocol.RemoteError as error:
            entry["error"] = {"code": error.code, "details": error.details}
            self.evidence["negotiations"].append(entry)
            raise
        self.session = protocol.validate_ready(reply, previous_session=self.session)
        self.summary = protocol.validate_init(init)
        entry["session"] = self.session
        self.evidence["negotiations"].append(entry)
        self.evidence["session"] = self.session
        if not self.evidence["subjects"]:
            self.evidence["subjects"] = [
                self._subject_entry(record) for record in self.records]
        return entry

    def _subject_entry(self, record):
        return {
            "id": record["id"],
            "root": record["root"],
            "namespace": record["namespace"],
            "bones": list(record["profile"]["bones"]),
            "parents": list(record["profile"]["parents"]),
            "curves": list(record["profile"]["curves"]),
            "curve_plugs": [list(plugs) for plugs in record["profile"]["curve_plugs"]],
            "meshes": list(record["profile"]["meshes"]),
            "ancestors": list(record["ancestors"]),
            "unit_scale": record["profile"]["unit_scale"],
            "bind_rows": len(record["wire"]["bind"]),
            "bind_conflict_count": record["profile"]["bind_conflict_count"],
        }

    def active_records(self):
        if self.summary is None:
            return list(self.records)
        active = set(self.summary["active"])
        return [record for record in self.records if record["id"] in active]

    # ----------------------------------------------------------------- frames

    def step(self, time_value):
        """Sample the active subjects at ``time_value`` and wait for the echo.

        ``time_value`` is a Maya source frame; it may move backwards or repeat,
        because the serial - not the time - identifies the evaluation.
        """
        records = self.active_records()
        scene.set_evaluated_time(time_value)
        if self.curve_overrides:
            # A keyed channel is rewritten by the evaluation, so the override is
            # re-applied for the frame that is about to be sampled.
            scene.reapply_curve_overrides(self.curve_overrides)
        serial = self.serial + 1
        direction = protocol.time_direction(self.last_time, float(time_value))
        subjects = []
        sent = []
        for record in records:
            transforms, curves = scene.sample_subject(record)
            subjects.append({"id": record["id"], "transforms": transforms,
                             "curves": curves})
            sent.append({
                "id": record["id"],
                "root": list(transforms[0]) if transforms else [],
                "curves": list(curves),
                "transforms": len(transforms),
                "digest": digest_transforms(transforms),
            })
        frame = protocol.make_frame(self.session, serial, float(time_value), subjects)
        protocol.validate_frame(frame, self.summary, session=self.session,
                                previous_serial=self.serial or None)
        started = time.monotonic()
        self._send(frame)
        reply = self._reply(protocol.APPLIED)
        latency_ms = (time.monotonic() - started) * 1000.0
        statuses = protocol.validate_applied(
            reply, self.session, serial, frame["time"],
            protocol.active_subject_ids(self.summary))
        self.serial = serial
        self.last_time = float(time_value)
        entry = {
            "session": self.session,
            "serial": serial,
            "time": float(time_value),
            "time_direction": direction,
            "latency_ms": latency_ms,
            "statuses": statuses,
            "subjects": sent,
        }
        self.evidence["frames"].append(entry)
        return entry

    def remove(self, subject_id):
        """Disable one subject; the other keeps streaming on this connection."""
        if self.session is None:
            raise protocol.ProtocolError(protocol.CODE_SESSION_INVALID,
                                         "negotiate before removing a subject")
        if subject_id not in self.summary["active"]:
            raise protocol.ProtocolError(
                protocol.CODE_REMOVE_ID_INACTIVE, "subject is not active",
                repr(subject_id))
        self._send(protocol.make_remove(self.session, subject_id))
        reply = self._reply(protocol.APPLIED)
        statuses = protocol.validate_remove_reply(
            reply, self.session, subject_id,
            protocol.active_subject_ids(self.summary),
            last_serial=self.serial or None,
            last_time=self.last_time)
        remaining = protocol.disable_subject(self.summary, subject_id)
        entry = {"id": subject_id, "statuses": statuses, "remaining": remaining,
                 "serial": reply.get("serial"), "time": reply.get("time")}
        self.evidence["removes"].append(entry)
        return entry


    def close(self):
        self.connection.close()

    def drop(self):
        self.connection.drop()


# ------------------------------------------------------------------ arguments

def _pair_ids(pair):
    parts = [part.strip() for part in pair.split("+") if part.strip()]
    if len(parts) != 2:
        raise ValueError("a pair is exactly two ids joined by '+', got {0!r}".format(pair))
    return parts


def _root_overrides(args):
    roots = {}
    for value in args.subject or []:
        if "=" not in value:
            raise ValueError("--subject needs id=root, got {0!r}".format(value))
        subject_id, root = value.split("=", 1)
        roots[subject_id.strip()] = root.strip()
    return roots


def _expectations(args):
    expectations = {}
    for option, key, split in (("expect_bones", "bones", ","),
                               ("expect_curves", "curves", ","),
                               ("expect_namespace", "namespace", None)):
        for value in getattr(args, option) or []:
            if "=" not in value:
                raise ValueError("--{0} needs id=value, got {1!r}".format(
                    option.replace("_", "-"), value))
            subject_id, payload = value.split("=", 1)
            if split:
                payload = [name.strip() for name in payload.split(split) if name.strip()]
            expectations.setdefault(subject_id.strip(), {})[key] = payload
    return expectations


def _curve_overrides(args):
    """``({subject_id: {curve: value}}, {subject_id: {curve: value}})``.

    The first mapping is `--set-curve` (refused when the plug is driven), the
    second is `--force-curve` (unlocks and disconnects, and says so in the
    evidence).
    """
    strict = {}
    forced = {}
    for option, target in (("set_curve", strict), ("force_curve", forced)):
        for value in getattr(args, option) or []:
            parts = [part.strip() for part in value.split("=")]
            if len(parts) != 3 or not parts[0] or not parts[1]:
                raise ValueError("--{0} needs id=curve=value, got {1!r}".format(
                    option.replace("_", "-"), value))
            try:
                number = float(parts[2])
            except ValueError:
                raise ValueError("--{0} value must be numeric, got {1!r}".format(
                    option.replace("_", "-"), value))
            target.setdefault(parts[0], {})[parts[1]] = number
    return strict, forced


def _spec_slug(recipe):
    name = str(recipe.get("name") or "rig-spec")
    return "".join(char if char.isalnum() or char in "-_" else "-" for char in name)


def _manifest_for(args):
    """Fixture manifest for the run: the checked-in recipe or ``--rig-spec``.

    A spec keeps its own fixture directory, so switching specs can never reuse
    another recipe's rig files; an existing manifest is reused only when its
    name and revision match the spec.
    """
    if args.rig_spec:
        recipe = scene.load_recipe(args.rig_spec)
        directory = Path(args.rig_dir) if args.rig_dir \
            else scene.scratch_directory() / _spec_slug(recipe)
        manifest = None
        if not args.regenerate_fixtures and (directory / "fixture.json").is_file():
            candidate = scene.load_manifest(directory)
            if candidate.get("name") == recipe.get("name") \
                    and candidate.get("revision") == recipe.get("revision"):
                manifest = candidate
        if manifest is None:
            manifest = scene.build_fixtures(recipe, directory=directory, force=True)
        return manifest
    if args.rig_dir and (Path(args.rig_dir) / "fixture.json").is_file() \
            and not args.regenerate_fixtures:
        return scene.load_manifest(args.rig_dir)
    return scene.build_fixtures(directory=args.rig_dir,
                                force=args.regenerate_fixtures)


def _probe_times(value):
    return [float(part) for part in str(value).split(",") if part.strip()]


# ------------------------------------------------------------------- probing

def build_records(manifest, pair, roots=None, expectations=None,
                  strict_ancestors=False):
    """Capture every subject of one pair and reject duplicate roots.

    A subject keeps its manifest fingerprint as the role expectation unless an
    explicit root override says the root is somebody else's, so a swapped role
    is refused instead of streamed.
    """
    roots = roots or {}
    expectations = expectations or {}
    records = []
    for rig_id in pair:
        root = roots.get(rig_id) or manifest["rigs"][rig_id]["root"]
        expect = expectations.get(rig_id)
        if expect is None and rig_id in manifest.get("fingerprints", {}) \
                and not roots.get(rig_id):
            expect = manifest["fingerprints"][rig_id]
        records.append(scene.capture_subject(
            rig_id, root, expectations=expect, strict_ancestors=strict_ancestors))
    scene.reject_duplicate_roots(records)
    return records


def probe(args):
    """Read-only inventory and sampling; never streams, never saves."""
    evidence = protocol.new_evidence(TOOL_NAME, host=args.host, port=args.port)
    session = None
    supplied_scene_digest = None
    if args.scene:
        supplied_scene_digest = scene.file_digest(args.scene)
        evidence["supplied_scene"] = {"file": args.scene,
                                      "before": supplied_scene_digest}
    try:
        manifest = _manifest_for(args)
        pair = _resolved_pair(args)
        rigs = list(args.reference_rig) if args.scene else list(pair)
        session = scene.SessionScene(manifest, rigs=rigs,
                                     scene_file=args.scene).open()
        wanted_fps = float(args.fps) if args.fps else float(manifest["fps"])
        units = scene.scene_units()
        evidence["scene"] = {
            "file": args.scene,
            "opened_clean": session.opened_clean,
            "fps": units["fps"],
            "linear_unit": units["linear_unit"],
            "requested_fps": wanted_fps,
            "references": session.reference_records(),
        }
        protocol.add_check(evidence, "scene uses centimetres",
                           units["linear_unit"] == "cm", units["linear_unit"])
        if args.fps:
            protocol.add_check(evidence, "scene frame rate matches --fps",
                               abs(units["fps"] - wanted_fps) <= 1.0e-9,
                               "{0} vs {1}".format(units["fps"], wanted_fps))
        records = build_records(
            manifest, _resolved_pair(args), roots=_root_overrides(args),
            expectations=_expectations(args),
            strict_ancestors=args.strict_ancestors)
        evidence["subjects"] = [
            {"id": record["id"], "root": record["root"],
             "namespace": record["namespace"],
             "bones": record["profile"]["bones"],
             "parents": record["profile"]["parents"],
             "curves": record["profile"]["curves"],
             "curve_plugs": record["profile"]["curve_plugs"],
             "meshes": record["profile"]["meshes"],
             "ancestors": record["ancestors"],
             "bind_rows": len(record["wire"]["bind"]),
             "bind_conflict_count": record["profile"]["bind_conflict_count"]}
            for record in records]
        protocol.add_check(evidence, "two subjects captured", len(records) == 2,
                           "captured {0}".format([r["id"] for r in records]))
        evidence["samples"] = []
        for time_value in args.probe_times:
            scene.set_evaluated_time(time_value)
            samples = {}
            for record in records:
                transforms, curves = scene.sample_subject(record)
                samples[record["id"]] = {
                    "root": list(transforms[0]) if transforms else [],
                    "curves": list(curves),
                    "transforms": len(transforms),
                    "digest": digest_transforms(transforms),
                }
            evidence["samples"].append({"time": float(time_value),
                                        "subjects": samples})
        evidence["cleanup"] = {"scene_saved": False}
    except scene.SceneRefused as error:
        protocol.add_error(evidence, error.code, error.details)
    except protocol.ProtocolError as error:
        protocol.add_error(evidence, error.code, error.details)
    finally:
        if session is not None and not args.leave_scene:
            session.close()
    if args.scene:
        try:
            after = scene.file_digest(args.scene)
            protocol.add_check(
                evidence, "supplied scene unchanged on disk",
                after["sha256"] == supplied_scene_digest["sha256"], args.scene)
            evidence.setdefault("supplied_scene", {"file": args.scene})["after"] = after
        except OSError as error:
            protocol.add_error(evidence, CODE_REFUSED,
                               "could not re-read the supplied scene: {0}".format(error))
    return _finish(evidence, args)


# ------------------------------------------------------------------ scenario

def run_scenario(args):
    evidence = protocol.new_evidence(TOOL_NAME, host=args.host, port=args.port)
    evidence["scenario"] = args.scenario
    manifest = _manifest_for(args)
    fps = float(args.fps) if args.fps else float(manifest["fps"])
    evidence["requested_fps"] = fps
    evidence["fixture"] = {
        "directory": manifest["directory"],
        "name": manifest.get("name"),
        "revision": manifest.get("revision"),
        "range": manifest["range"],
        "fps": manifest["fps"],
        "rigs": {rig_id: {"file": rig["file"], "root": rig["root"],
                          "bones": rig["bones"], "curves": rig["curves"]}
                 for rig_id, rig in sorted(manifest["rigs"].items())},
    }
    digests_before = scene.fixture_digests(manifest)
    supplied_scene_digest = None
    if args.scene:
        supplied_scene_digest = scene.file_digest(args.scene)
        evidence["supplied_scene"] = {"file": args.scene, "before": supplied_scene_digest}

    modified_before_close = None
    try:
        for pair in _scenario_pairs(args):
            _run_pair(args, evidence, manifest, pair, fps)
    except scene.SceneRefused as error:
        protocol.add_error(evidence, error.code, error.details)
    except protocol.ProtocolError as error:
        protocol.add_error(evidence, error.code, error.details)
    except protocol.RemoteError as error:
        protocol.add_error(evidence, error.code, error.details)
    except ValueError as error:
        protocol.add_error(evidence, CODE_SCENARIO_EXPECTATION, str(error))
    finally:
        try:
            if not args.leave_scene:
                modified_before_close = bool(scene.cmds().file(query=True, modified=True))
                scene.cmds().file(new=True, force=True)
        except Exception:  # never mask a session error with a cleanup error
            pass

    try:
        digests_after = scene.fixture_digests(manifest)
        protocol.add_check(
            evidence, "generated rig files unchanged by the session",
            digests_after == digests_before,
            sorted(digests_before))
        if args.scene:
            protocol.add_check(
                evidence, "supplied scene unchanged on disk",
                scene.file_digest(args.scene)["sha256"] == supplied_scene_digest["sha256"],
                args.scene)
        evidence["cleanup"] = {
            "scene_saved": False,
            "session_scene_closed": not args.leave_scene,
            "scene_modified_before_close": modified_before_close,
            "kept_open": bool(args.leave_scene),
        }
    except Exception as error:
        protocol.add_error(evidence, CODE_REFUSED,
                           "post-session verification failed: {0}".format(error))
    return _finish(evidence, args)


def _resolved_pair(args):
    """The pair a single-scene mode drives: explicit or the scenario's pair.

    The Unreal fixture registers the ids it serves per scenario: ``character`` +
    ``prop`` for the mandatory pair and ``character`` + ``arms`` for the
    replacement pair, so the arms subject takes the replaced role inside its own
    pair rather than being paired with the prop.
    """
    if args.pair:
        return _pair_ids(args.pair)
    if args.scenario == "character-arms":
        return ["character", "arms"]
    return ["character", "prop"]


def _scenario_pairs(args):
    """Pairs of one scenario.

    ``role-replace`` drives its own two-pair sequence (character+prop, then
    character+arms, each on its own connection) and refuses an explicit
    ``--pair`` rather than silently ignoring it; the other scenarios take
    ``--pair`` or their default pair.
    """
    if args.scenario == "role-replace":
        if args.pair:
            raise ValueError(
                "--scenario role-replace drives character+prop then character+arms; "
                "--pair is not used with it")
        return [["character", "prop"], ["character", "arms"]]
    return [_resolved_pair(args)]


def _run_pair(args, evidence, manifest, pair, fps):
    roots = {subject_id: root for subject_id, root in _root_overrides(args).items()
             if subject_id in pair}
    # With a supplied scene, only the rigs the caller asked to reference are
    # added to it; the subjects a supplied scene already provides need no
    # generated rig at all.
    if args.scene and args.reference_rig:
        rigs = [rig_id for rig_id in args.reference_rig if rig_id in manifest["rigs"]]
    else:
        rigs = [rig_id for rig_id in pair if rig_id in manifest["rigs"]]
    session = scene.SessionScene(manifest, rigs=rigs, scene_file=args.scene).open()
    driver = None
    try:
        scene_fps = scene.require_scene_units(fps)
        references = session.reference_records()
        expected_references = list(rigs)
        evidence.setdefault("scenes", []).append({
            "pair": list(pair),
            "file": args.scene,
            "opened_clean": session.opened_clean,
            "fps": scene_fps,
            "linear_unit": scene.cmds().currentUnit(query=True, linear=True),
            "references": references,
        })
        protocol.add_check(
            evidence, "references present for {0}".format("+".join(pair)),
            all(any(record["namespace"].lstrip(":") == manifest["rigs"][rig_id]["namespace"]
                    for record in references) for rig_id in expected_references),
            "expected {0}, found {1}".format(
                expected_references, [record["namespace"] for record in references]))
        records = build_records(manifest, pair, roots=roots,
                                expectations=_expectations(args),
                                strict_ancestors=args.strict_ancestors)
        curve_overrides, forced_curves = _curve_overrides(args)
        if curve_overrides or forced_curves:
            evidence["curve_overrides"] = scene.apply_curve_overrides(
                records, curve_overrides, forced_curves)
        driver = MultiSubjectSession(records, args.host, args.port, fps,
                                     timeout=args.timeout).connect()
        driver.curve_overrides = evidence.get("curve_overrides") or []
        substitute = None
        if args.scenario == "character-arms":
            # The replacement scenario declares the arms subject with the
            # full-body sibling's skeleton, so the receiver's own target check
            # refuses it while the character subject stays correct.
            full_body = next((record for record in records
                              if record["id"] == "character"), None)
            if full_body is not None:
                substitute = full_body["wire"]
        _negotiate(driver, args, evidence, pair, substitute)
        start_time = float(args.start_frame) if args.start_frame is not None \
            else float(manifest["range"][0])
        results = ready_frames(driver, args, driver.records, start_time)
        expected_frames = int(args.frames)
        if args.drop_after is not None:
            expected_frames = min(expected_frames, int(args.drop_after))
        protocol.add_check(
            evidence, "frames applied for {0}".format("+".join(pair)),
            results["frames"] == expected_frames,
            json.dumps(results))
        if args.remove_at is not None:
            protocol.add_check(
                evidence, "subject removed and the other kept streaming",
                results["removed"] is not None,
                json.dumps(results))
        if args.scenario == "character-arms":
            _scenario_expectations(evidence, driver)
        if args.times:
            planned = _probe_times(args.times)[:expected_frames]
            expected_directions = []
            previous = None
            for value in planned:
                expected_directions.append(protocol.time_direction(previous, float(value)))
                previous = float(value)
            observed = results["directions"][:len(expected_directions)]
            protocol.add_check(
                evidence, "the planned reverse/re-edit source times were streamed",
                observed == expected_directions,
                json.dumps({"planned": planned, "expected": expected_directions,
                            "observed": results["directions"]}))
    finally:
        if driver is not None:
            driver.close()
            evidence["sessions"].append(driver.evidence)
        if not args.leave_scene:
            scenes = evidence.get("scenes") or []
            if scenes:
                scenes[-1]["modified_before_close"] = bool(
                    scene.cmds().file(query=True, modified=True))
            session.close()


def ready_frames(driver, args, records, start_time):
    """Stream the planned frames and honour ``--remove-at``/``--drop-after``.

    ``--times`` lists one Maya source frame per step, so a reverse scrub or a
    re-edit of an already sent frame can be exercised; without it the times run
    forward from ``start_time`` by ``--step``.
    """
    results = {"frames": 0, "removed": None, "dropped": None, "directions": []}
    planned = _probe_times(args.times) if args.times else []
    time_value = float(start_time)
    for index in range(int(args.frames)):
        if args.drop_after is not None and results["frames"] >= int(args.drop_after):
            driver.drop()
            results["dropped"] = results["frames"]
            return results
        step_time = float(planned[index]) if index < len(planned) else time_value
        frame = driver.step(step_time)
        results["frames"] += 1
        results["directions"].append(frame["time_direction"])
        if args.remove_at is not None \
                and results["frames"] == int(args.remove_at) \
                and results["removed"] is None:
            target = args.remove_id or records[1]["id"]
            driver.remove(target)
            results["removed"] = target
        time_value = step_time + float(args.step)
    if args.drop_after is not None and results["frames"] >= int(args.drop_after):
        driver.drop()
        results["dropped"] = results["frames"]
    return results


def _negotiate(driver, args, evidence, pair, substitute=None):
    if args.scenario == "character-arms":
        # The arms subject is declared with the full-body skeleton; every other
        # subject is declared correctly.
        mismatched = [
            scene.mismatched_subject(record, substitute=substitute)
            if record["id"] == "arms" and substitute is not None
            else dict(record["wire"])
            for record in driver.records]
        try:
            driver.negotiate(wires=mismatched, note="deliberately mismatched init")
        except protocol.RemoteError as error:
            evidence.setdefault("refusals", []).append(
                {"note": "deliberately mismatched init", "code": error.code,
                 "details": error.details, "pair": list(pair)})
            protocol.add_check(
                evidence, "mismatched init was refused",
                bool(error.code), "code={0!r}".format(error.code))
        else:
            protocol.add_error(
                evidence, CODE_SCENARIO_EXPECTATION,
                "the receiver accepted a deliberately mismatched arms skeleton")
    driver.negotiate(note="negotiated init")


def _scenario_expectations(evidence, driver):
    negotiations = driver.evidence["negotiations"]
    refused = [entry for entry in negotiations if entry["error"]]
    sessions = [entry["session"] for entry in negotiations if entry["session"]]
    protocol.add_check(
        evidence, "the refused init came first and applied no pose",
        bool(negotiations) and negotiations[0]["error"] is not None
        and len(refused) == 1
        and not [entry for entry in negotiations[1:] if entry["error"]],
        json.dumps(negotiations))
    protocol.add_check(
        evidence, "the fresh init after the refusal negotiated a session",
        len(sessions) == 1 and sessions[0] >= 1
        and negotiations[-1]["error"] is None,
        "sessions={0}".format(sessions))


def _finish(evidence, args):
    protocol.finalise_evidence(evidence)
    if args.evidence:
        target = Path(args.evidence)
        target.parent.mkdir(parents=True, exist_ok=True)
        protocol.write_json(str(target), evidence)
    print(json.dumps(evidence, indent=2, ensure_ascii=True))
    return 0 if evidence["ok"] else 1


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=protocol.DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=protocol.DEFAULT_PORT)
    parser.add_argument("--scenario", choices=SCENARIOS, default="character-prop")
    parser.add_argument("--pair", default=None,
                        help="two subject ids joined by '+'; defaults to the scenario's pair")
    parser.add_argument("--frames", type=int, default=4)
    parser.add_argument("--fps", type=float, default=None,
                        help="must match the fixture/scene frame rate; defaults to it")
    parser.add_argument("--start-frame", type=float, default=None)
    parser.add_argument("--times", default=None,
                        help="comma-separated Maya source frames, one per step; "
                             "a repeated or decreasing value exercises a re-edit "
                             "or a reverse scrub")
    parser.add_argument("--set-curve", action="append", default=[],
                        help="id=curve=value set on the disposable session scene "
                             "before sampling, so a Morph channel carries a known "
                             "value (repeatable)")
    parser.add_argument("--force-curve", action="append", default=[],
                        help="id=curve=value like --set-curve, but unlocks and "
                             "disconnects a driven plug and records that in the "
                             "evidence (repeatable)")
    parser.add_argument("--step", type=float, default=1.0)
    parser.add_argument("--remove-at", type=int, default=None,
                        help="after this 1-based frame, disable the second subject")
    parser.add_argument("--remove-id", default=None,
                        help="subject disabled by --remove-at (default: the second)")
    parser.add_argument("--drop-after", type=int, default=None,
                        help="close the connection abruptly after this many frames")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--strict-ancestors", action="store_true",
                        help="refuse a subject whose Maya root has an animated ancestor")
    parser.add_argument("--rig-dir", default=None,
                        help="fixture directory; defaults to the .tmp scratch directory")
    parser.add_argument("--rig-spec", default=None,
                        help="recipe JSON to build instead of the checked-in one, for "
                             "matching another host's target skeletons")
    parser.add_argument("--regenerate-fixtures", action="store_true")
    parser.add_argument("--write-fixtures", action="store_true",
                        help="only materialise the fixture recipe, then exit")
    parser.add_argument("--scene", default=None,
                        help="open a supplied scene read-only instead of a fresh one")
    parser.add_argument("--reference-rig", action="append", default=[],
                        help="fixture rig to reference into --scene (repeatable)")
    parser.add_argument("--subject", action="append", default=[],
                        help="explicit root override id=root (repeatable)")
    parser.add_argument("--expect-bones", action="append", default=[],
                        help="role fingerprint id=bone,bone (repeatable)")
    parser.add_argument("--expect-curves", action="append", default=[],
                        help="role fingerprint id=curve,curve (repeatable)")
    parser.add_argument("--expect-namespace", action="append", default=[],
                        help="role reference namespace id=namespace (repeatable)")
    parser.add_argument("--probe", action="store_true",
                        help="read-only inventory and sampling, no streaming")
    parser.add_argument("--probe-times", type=_probe_times, default=(1.0, 3.0, 5.25))
    parser.add_argument("--leave-scene", action="store_true",
                        help="keep the session scene open instead of discarding it")
    parser.add_argument("--evidence", default=None,
                        help="path for the JSON evidence file (UE automation reads it)")
    args = parser.parse_args(argv)

    try:
        if args.write_fixtures:
            manifest = _manifest_for(args)
            print(json.dumps({
                "ok": True,
                "fixture": manifest["directory"],
                "manifest": str(Path(manifest["directory"]) / "fixture.json"),
                "rigs": {rig_id: rig["root"]
                         for rig_id, rig in sorted(manifest["rigs"].items())},
            }, indent=2, ensure_ascii=True))
            return 0
        if args.probe:
            return probe(args)
        return run_scenario(args)
    except scene.SceneRefused as error:
        return _refused(args, error.code, error.details)
    except protocol.ProtocolError as error:
        return _refused(args, error.code, error.details)
    except ValueError as error:
        return _refused(args, CODE_SCENARIO_EXPECTATION, str(error))


def _refused(args, code, details):
    evidence = protocol.new_evidence(TOOL_NAME, host=args.host, port=args.port)
    protocol.add_error(evidence, code, details)
    return _finish(evidence, args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:  # a crash must still be readable in mayapy logs
        traceback.print_exc()
        raise SystemExit(2)
