"""Standalone mayapy peer: drive a real Unreal receiver and write evidence.

This is the artifact an Unreal automation test invokes. It is a thin wrapper
around the prototype's Maya entry point, so there is exactly one session
implementation. Flags follow the Unreal adapter's documented names:

    mayapy maya_peer.py --host 127.0.0.1 --port 54340 \
        --scenario character-prop --frames 4 --fps 30 --start-frame 1 \
        --evidence peer_evidence.json [--times 1,3,2,2]

``--times`` sends an explicit Maya source frame per step, which is how a reverse
scrub and a re-edit of an already sent frame are exercised end to end.

Scenarios:

* ``character-prop``: two-subject pair, one evaluated Maya time per serial;
* ``character-arms``: a deliberately mismatched arms init must be refused, then
  a fresh init on the same connection must be accepted with a new session id;
* ``role-replace``: character+prop, then arms+prop on a new connection.

``--spawn-mock`` runs the bundled stand-in receiver in this process instead of
connecting to Unreal, which is how the self-contained smoke run works.

Exit code 0 means the scenario's expected outcome happened; the evidence JSON
is written in both cases.
"""

import argparse
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SCRIPTS = ROOT / "scripts"
for entry in (str(HERE), str(SCRIPTS)):
    if entry not in sys.path:
        sys.path.insert(0, entry)

import mtou_multi_subject_scene as scene  # noqa: E402


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--scenario", default="character-prop",
                        choices=("character-prop", "character-arms", "role-replace"))
    parser.add_argument("--pair", default=None,
                        help="two subject ids joined by '+'; defaults to the scenario's pair")
    parser.add_argument("--frames", type=int, default=4)
    parser.add_argument("--fps", type=float, default=None)
    parser.add_argument("--start-frame", type=float, default=None)
    parser.add_argument("--step", type=float, default=1.0)
    parser.add_argument("--remove-at", type=int, default=None)
    parser.add_argument("--remove-id", default=None)
    parser.add_argument("--drop-after", type=int, default=None)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--times", default=None,
                        help="comma-separated Maya source frames, one per step; "
                             "a repeated or decreasing value exercises a re-edit "
                             "or a reverse scrub")
    parser.add_argument("--strict-ancestors", action="store_true")
    parser.add_argument("--rig-dir", default=None)
    parser.add_argument("--regenerate-fixtures", action="store_true")
    parser.add_argument("--write-fixtures", action="store_true")
    parser.add_argument("--scene", default=None)
    parser.add_argument("--reference-rig", action="append", default=[])
    parser.add_argument("--subject", action="append", default=[])
    parser.add_argument("--expect-bones", action="append", default=[])
    parser.add_argument("--expect-curves", action="append", default=[])
    parser.add_argument("--expect-namespace", action="append", default=[])
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--probe-times", default=None)
    parser.add_argument("--leave-scene", action="store_true")
    parser.add_argument("--evidence", default=None)
    parser.add_argument("--spawn-mock", action="store_true",
                        help="run the bundled stand-in receiver in this process")
    parser.add_argument("--mock-fault", action="append", default=[],
                        help="id=value fault for --spawn-mock (repeatable)")
    args = parser.parse_args(argv)

    entry = load_module("MtoUMultiSubjectPrototype", SCRIPTS / "MtoUMultiSubjectPrototype.py")
    mock = None
    forwarded = [
        "--scenario", args.scenario,
        "--frames", str(args.frames),
        "--step", repr(args.step),
        "--timeout", repr(args.timeout),
    ]
    if args.pair:
        forwarded += ["--pair", args.pair]
    if args.host:
        forwarded += ["--host", args.host]
    if args.port is not None:
        forwarded += ["--port", str(args.port)]
    if args.fps is not None:
        forwarded += ["--fps", repr(args.fps)]
    if args.start_frame is not None:
        forwarded += ["--start-frame", repr(args.start_frame)]
    if args.times:
        forwarded += ["--times", args.times]
    if args.remove_at is not None:
        forwarded += ["--remove-at", str(args.remove_at)]
    if args.remove_id:
        forwarded += ["--remove-id", args.remove_id]
    if args.drop_after is not None:
        forwarded += ["--drop-after", str(args.drop_after)]
    if args.strict_ancestors:
        forwarded.append("--strict-ancestors")
    if args.rig_dir:
        forwarded += ["--rig-dir", args.rig_dir]
    if args.regenerate_fixtures:
        forwarded.append("--regenerate-fixtures")
    if args.write_fixtures:
        forwarded.append("--write-fixtures")
    if args.scene:
        forwarded += ["--scene", args.scene]
    for value in args.reference_rig:
        forwarded += ["--reference-rig", value]
    for value in args.subject:
        forwarded += ["--subject", value]
    for value in args.expect_bones:
        forwarded += ["--expect-bones", value]
    for value in args.expect_curves:
        forwarded += ["--expect-curves", value]
    for value in args.expect_namespace:
        forwarded += ["--expect-namespace", value]
    if args.probe:
        forwarded.append("--probe")
    if args.probe_times:
        forwarded += ["--probe-times", args.probe_times]
    if args.leave_scene:
        forwarded.append("--leave-scene")
    if args.evidence:
        forwarded += ["--evidence", args.evidence]

    try:
        if args.spawn_mock:
            mock_module = load_module("mock_multi_subject_receiver",
                                      HERE / "mock_multi_subject_receiver.py")
            fixture_dir = _fixture_directory(args, entry)
            fingerprints = mock_module.load_fingerprints(
                Path(fixture_dir) / "fixture.json")
            faults = {}
            for value in args.mock_fault:
                if "=" not in value:
                    raise SystemExit("--mock-fault needs id=value, got {0!r}".format(value))
                key, payload = value.split("=", 1)
                if key in ("serial_offset", "session_offset", "close_after_frames"):
                    faults[key] = int(payload)
                elif key == "time_offset":
                    faults[key] = float(payload)
                elif key == "silence_frames":
                    faults[key] = tuple(int(part) for part in payload.split(",")
                                        if part.strip())
                elif key in ("refuse_inits", "allow_reinit"):
                    faults[key] = bool(int(payload))
                else:
                    raise SystemExit("unknown --mock-fault {0!r}".format(key))
            mock = mock_module.MockReceiver(fingerprints, faults).start()
            forwarded += ["--host", mock.host, "--port", str(mock.port)]
        if "--host" not in forwarded:
            forwarded += ["--host", "127.0.0.1"]
        if "--port" not in forwarded:
            forwarded += ["--port", "54340"]
        return entry.main(forwarded)
    finally:
        if mock is not None:
            mock.stop()


def _fixture_directory(args, entry):
    """Fixture directory, materialising the recipe when it is not there yet."""
    if args.rig_dir and (Path(args.rig_dir) / "fixture.json").is_file():
        return args.rig_dir
    manifest_args = ["--write-fixtures"]
    if args.rig_dir:
        manifest_args += ["--rig-dir", args.rig_dir]
    if args.regenerate_fixtures:
        manifest_args.append("--regenerate-fixtures")
    if entry.main(manifest_args) != 0:
        raise SystemExit("could not materialise the fixture recipe")
    return args.rig_dir or str(scene.scratch_directory())


if __name__ == "__main__":
    raise SystemExit(main())
