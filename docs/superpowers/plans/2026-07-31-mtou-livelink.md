# MtoU_LiveLink Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the approved Windows-only Maya 2022.4 to Unreal Engine 5.7.4 live character-preview bridge, including evaluated bones, matching BlendShape curves, a native Live Link source, a user-created binding asset, and drag-to-level placement.

**Architecture:** One directly runnable Maya Python 3.7 file samples the selected deformation hierarchy on Maya's main thread at 30 Hz, serializes the newest pose as length-prefixed compact JSON, and hands it to one standard-library TCP sender thread. One Unreal Runtime module accepts a single loopback client, validates the static hierarchy against every placed binding actor, and publishes the fixed `MtoU_Character` subject through `ILiveLinkClient`; one Editor module supplies only the binding and actor factories.

**Tech Stack:** Python 3.7 standard library, Maya Python API 2.0 / `maya.cmds`, Unreal Engine 5.7.4 C++, `Sockets`, `Json`, `LiveLinkInterface`, `LiveLinkAnimationCore`, `UnrealEd`, Python `unittest`, and Unreal Automation Tests.

## Global Constraints

- Windows 64-bit only.
- Autodesk Maya 2022.4, using its bundled Python 3.7 and Maya API version `20220400`.
- Stock Unreal Engine 5.7.4 is the baseline; Unreal Editor only.
- The user's third-party-modified UE 5.7 build remains unclaimed until the same build and acceptance checks pass there.
- Product and `FriendlyName`: `MtoU_LiveLink`; technical project/module names: `MtoULiveLink` and `MtoULiveLinkEditor`.
- Both independently packageable projects start at version `0.1.0`.
- Add no third-party production dependency and no new repository dependency.
- Endpoint `127.0.0.1:54321`, one TCP client, protocol version `1`, subject `MtoU_Character`.
- One Maya character only; no retargeting, partial skeletons, cross-machine transport, packaged-game support, automatic reconnect, binary protocol, or per-node dirty callbacks.
- Maya must not edit the scene, execute or modify either MEL export script, bake animation, merge namespaces, reparent nodes, delete nodes, or export FBX.
- Preserve the placed Unreal Actor transform; root motion remains a root-bone transform in the Live Link pose.
- Do not add an application-level bone, curve, or message-size limit. Reject only values that cannot be represented by Unreal's `int32`-indexed containers.
- Project READMEs contain only introduction, supported versions, installation, and usage, with matching English and Chinese information.
- Do not commit `Binaries`, `Intermediate`, `Saved`, caches, generated project files, archives, or user-created binding assets.

---

## Protocol Contract

Every packet is UTF-8 JSON preceded by an unsigned 64-bit big-endian payload length. Use these exact schemas so the two projects can be implemented and tested independently:

```json
{"type":"init","version":1,"bones":[["root",-1],["spine_01",0]],"curves":["Smile"]}
```

```json
{"type":"ready","missing_curves":["Blink_R"]}
```

```json
{"type":"error","message":"No placed MtoU_LiveLink Binding actor was found."}
```

```json
{"type":"frame","transforms":[[0.0,0.0,0.0,0.0,0.0,0.0,1.0,1.0,1.0,1.0]],"curves":[0.5]}
```

- Each bone record is `[normalized_name, parent_index]`; index `0` is the sole root and has parent `-1`, and every later parent index is smaller than its child index.
- Each transform is `[tx, ty, tz, qx, qy, qz, qw, sx, sy, sz]` in Unreal centimeters/basis.
- `frame.curves` remains aligned to all curve names announced by Maya. Unreal keeps an accepted-index list and publishes only values whose names are Morph Targets on every participating Skeletal Mesh.
- Maya waits for `ready` before sending the first frame. A handshake `error` is shown in Maya and closes the connection.
- A structural message error closes the connection. A non-finite frame value is logged with its bone/curve index and dropped without closing the session.

## File Map

### Maya project

- `maya/tools/MtoULiveLink/scripts/MtoULiveLink.py` — sole runtime file; pure protocol/conversion helpers, Maya discovery/sampling, sender thread, lifecycle, UI, and `run()`.
- `maya/tools/MtoULiveLink/tests/test_mtou_livelink.py` — pure standard-library tests runnable under repository Python and `mayapy`.
- `maya/tools/MtoULiveLink/tests/test_maya_host.py` — Maya-standalone sampling/discovery smoke tests; skipped outside Maya.
- `maya/tools/MtoULiveLink/README.md` and `README_CN.md` — matching user-facing introduction, versions, installation, and usage.
- `maya/tools/MtoULiveLink/CHANGELOG.md` and `LICENSE` — project release history and repository license copy.

### Unreal project

- `unreal/Plugins/MtoULiveLink/MtoULiveLink.uplugin` — version, Win64/Editor restriction, modules, and Live Link dependency.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/MtoULiveLink.Build.cs` — Runtime module dependencies.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Public/MtoULiveLinkBinding.h` — `UMtoULiveLinkBinding` data asset with one Skeletal Mesh reference.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Public/MtoULiveLinkActor.h` — placed actor, Skeletal Mesh Component, binding, and read-only transient status.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkActor.cpp` — binding application and native `ULiveLinkInstance` setup.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkProtocol.h/.cpp` — framing, JSON parsing, finite/count checks, hierarchy diagnostics, and Live Link data construction.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkSource.h/.cpp` — loopback listener, one connection, worker, newest-frame handoff, actor scan, validation, and Live Link publication.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkModule.cpp` — idempotent source registration and shutdown.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/Tests/MtoULiveLinkTests.cpp` — Runtime automation tests.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor/MtoULiveLinkEditor.Build.cs` — Editor-only dependencies.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor/Private/MtoULiveLinkFactories.h/.cpp` — binding factory and binding-to-actor factory.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor/Private/MtoULiveLinkEditorModule.cpp` — one-line default Editor module implementation.
- `unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor/Private/Tests/MtoULiveLinkEditorTests.cpp` — factory automation smoke tests.
- `unreal/Plugins/MtoULiveLink/README.md`, `README_CN.md`, `CHANGELOG.md`, and `LICENSE` — independently installable plugin metadata.

### Repository integration

- `unreal/ToolsLab.uproject` — enable `MtoULiveLink` in the validation host.
- `README.md`, `README_CN.md`, and `CHANGELOG.md` — add both independently installable sides to the repository index/history.

---

### Task 1: Maya pure protocol and conversion core

**Files:**
- Create from template: `maya/tools/MtoULiveLink/`
- Modify: `maya/tools/MtoULiveLink/scripts/MtoULiveLink.py`
- Replace test: `maya/tools/MtoULiveLink/tests/test_package_layout.py` -> `maya/tools/MtoULiveLink/tests/test_mtou_livelink.py`

**Interfaces:**
- Consumes: Python 3.7 standard library only.
- Produces: `normalize_name(path) -> str`, `build_hierarchy(root, children) -> list[dict]`, `centimeters_per_unit(unit) -> float`, `convert_transform(translation, quaternion, scale, unit_scale) -> list[float]`, `encode_message(message) -> bytes`, `recv_message(sock) -> dict`, `make_init_message(bones, curves) -> dict`, and `make_frame_message(transforms, curves) -> dict`.

- [ ] **Step 1: Reuse the repository scaffold**

Run:

```powershell
python tools/create_project.py maya-tool MtoULiveLink --json
python tools/create_project.py maya-tool MtoULiveLink --apply --json
```

Expected: the preview and apply results list `maya/tools/MtoULiveLink`, the runtime file, bilingual READMEs, changelog, test, and license; no `.mod` file is created.

- [ ] **Step 2: Replace the template test with failing protocol tests**

Delete `test_package_layout.py` and create `test_mtou_livelink.py` with this test surface:

```python
import importlib.util
import math
import pathlib
import struct
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "MtoULiveLink.py"
SPEC = importlib.util.spec_from_file_location("MtoULiveLink", str(SCRIPT))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ProtocolTests(unittest.TestCase):
    def test_namespace_normalization(self):
        self.assertEqual("spine_01", MODULE.normalize_name("|Rig|Hero:spine_01"))
        self.assertEqual("jaw", MODULE.normalize_name("|Rig|show:Hero:jaw"))

    def test_hierarchy_is_parent_first_and_rejects_duplicates(self):
        children = {"|root": ["|root|a", "|root|b"], "|root|a": ["|root|a|tip"]}
        records = MODULE.build_hierarchy("|root", lambda path: children.get(path, []))
        self.assertEqual(["root", "a", "tip", "b"], [record["name"] for record in records])
        self.assertEqual([-1, 0, 1, 0], [record["parent"] for record in records])
        with self.assertRaisesRegex(ValueError, "duplicate normalized bone names: arm"):
            MODULE.build_hierarchy("|root", lambda path: ["|root|A:arm", "|root|B:arm"] if path == "|root" else [])

    def test_units_and_basis_conversion(self):
        self.assertEqual(100.0, MODULE.centimeters_per_unit("m"))
        transform = MODULE.convert_transform((1, 2, 3), (0, 0, 0, 1), (1, 2, 3), 100.0)
        self.assertEqual([100.0, 300.0, 200.0], transform[:3])
        self.assertEqual([1.0, 3.0, 2.0], transform[7:])
        half = math.sqrt(0.5)
        rotated = MODULE.convert_transform((0, 0, 0), (0, half, 0, half), (1, 1, 1), 1.0)
        self.assertAlmostEqual(-half, rotated[5])
        self.assertAlmostEqual(half, rotated[6])

    def test_length_prefix_and_large_dynamic_message(self):
        bones = [["bone_{0}".format(index), index - 1] for index in range(701)]
        packet = MODULE.encode_message(MODULE.make_init_message(bones, []))
        self.assertEqual(len(packet) - 8, struct.unpack(">Q", packet[:8])[0])
        self.assertIn(b"bone_700", packet)

    def test_non_finite_frame_is_rejected(self):
        for value in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "finite"):
                    MODULE.make_frame_message([[0, 0, 0, 0, 0, 0, 1, 1, 1, value]], [])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 3: Run the test to verify it fails**

Run:

```powershell
python -m unittest discover -s maya/tools/MtoULiveLink/tests -p "test_mtou_livelink.py" -v
```

Expected: FAIL because `normalize_name` and the other protocol functions do not exist.

- [ ] **Step 4: Implement the minimal pure core at the top of `MtoULiveLink.py`**

Use these constants and algorithms; keep Maya imports guarded so repository Python can import the file:

```python
import json
import math
import socket
import struct


__version__ = "0.1.0"
PROTOCOL_VERSION = 1
HOST = "127.0.0.1"
PORT = 54321
SUBJECT_NAME = "MtoU_Character"

try:
    import maya.api.OpenMaya as om
    import maya.cmds as cmds
except ImportError:
    om = None
    cmds = None


def normalize_name(path):
    return path.rsplit("|", 1)[-1].rsplit(":", 1)[-1]


def build_hierarchy(root, children):
    records = []
    stack = [(root, -1)]
    while stack:
        path, parent = stack.pop()
        index = len(records)
        records.append({"path": path, "name": normalize_name(path), "parent": parent})
        child_paths = list(children(path) or [])
        stack.extend((child, index) for child in reversed(child_paths))
    counts = {}
    for record in records:
        counts[record["name"]] = counts.get(record["name"], 0) + 1
    duplicates = sorted(name for name, count in counts.items() if count > 1)
    if duplicates:
        raise ValueError("duplicate normalized bone names: {0}".format(", ".join(duplicates)))
    return records


def centimeters_per_unit(unit):
    factors = {"mm": 0.1, "cm": 1.0, "m": 100.0, "km": 100000.0,
               "in": 2.54, "ft": 30.48, "yd": 91.44, "mi": 160934.4}
    if unit not in factors:
        raise ValueError("unsupported Maya linear unit: {0}".format(unit))
    return factors[unit]


def _finite(values):
    return all(math.isfinite(float(value)) for value in values)


def convert_transform(translation, quaternion, scale, unit_scale):
    qx, qy, qz, qw = (float(value) for value in quaternion)
    length = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
    if not length or not _finite(translation) or not _finite(quaternion) or not _finite(scale):
        raise ValueError("transform values must be finite and quaternion length must be non-zero")
    qx, qy, qz, qw = qx / length, qy / length, qz / length, qw / length
    tx, ty, tz = (float(value) * unit_scale for value in translation)
    sx, sy, sz = (float(value) for value in scale)
    return [tx, tz, ty, -qx, -qz, -qy, qw, sx, sz, sy]


def make_init_message(bones, curves):
    return {"type": "init", "version": PROTOCOL_VERSION, "bones": bones, "curves": curves}


def make_frame_message(transforms, curves):
    flat = [value for transform in transforms for value in transform] + list(curves)
    if not _finite(flat):
        raise ValueError("frame values must be finite")
    return {"type": "frame", "transforms": transforms, "curves": curves}


def encode_message(message):
    payload = json.dumps(message, ensure_ascii=False, allow_nan=False,
                         separators=(",", ":")).encode("utf-8")
    return struct.pack(">Q", len(payload)) + payload


def _recv_exact(sock, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise EOFError("connection closed while receiving a protocol message")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_message(sock):
    size = struct.unpack(">Q", _recv_exact(sock, 8))[0]
    return json.loads(_recv_exact(sock, size).decode("utf-8"))
```

The basis change is Maya `(x, y, z)` to Unreal `(x, z, y)`. Because that axis swap changes handedness, map the quaternion pseudovector to `(-x, -z, -y)` and preserve `w`; normalize before transmission.

- [ ] **Step 5: Run the pure tests and repository validation**

Run:

```powershell
python -m unittest discover -s maya/tools/MtoULiveLink/tests -v
python tools/validate_repository.py
```

Expected: both commands PASS.

- [ ] **Step 6: Commit the pure Maya core**

```powershell
git add maya/tools/MtoULiveLink
git commit -m "feat(maya): add MtoU Live Link protocol core"
```

---

### Task 2: Maya hierarchy, BlendShape discovery, and evaluated sampling

**Files:**
- Modify: `maya/tools/MtoULiveLink/scripts/MtoULiveLink.py`
- Create: `maya/tools/MtoULiveLink/tests/test_maya_host.py`

**Interfaces:**
- Consumes: Task 1's `build_hierarchy`, `centimeters_per_unit`, and `convert_transform`.
- Produces: `_capture_subject() -> dict` and `_sample_pose(subject) -> (list[list[float]], list[float])`. The subject contains `root`, parent-first `bones`, ordered `curves`, and one unit scale captured at connection.

- [ ] **Step 1: Write the Maya-host smoke test**

```python
import importlib.util
import pathlib
import unittest

try:
    import maya.standalone
    MAYA_AVAILABLE = True
except ImportError:
    MAYA_AVAILABLE = False


@unittest.skipUnless(MAYA_AVAILABLE, "requires Maya mayapy")
class MayaHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        maya.standalone.initialize(name="python")
        global cmds, module
        import maya.cmds as cmds
        script = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "MtoULiveLink.py"
        spec = importlib.util.spec_from_file_location("MtoULiveLinkHost", str(script))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

    @classmethod
    def tearDownClass(cls):
        maya.standalone.uninitialize()

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm")

    def test_captures_evaluated_joints_and_blendshape_alias(self):
        cmds.namespace(add="Hero")
        root = cmds.joint(name="Hero:root", position=(1, 2, 3))
        child = cmds.joint(name="Hero:spine", position=(1, 5, 3))
        base = cmds.polyCube(name="body")[0]
        target = cmds.duplicate(base, name="smileTarget")[0]
        blendshape = cmds.blendShape(target, base, name="faceBS")[0]
        cmds.aliasAttr("Smile", blendshape + ".weight[0]")
        cmds.skinCluster(root, base, name="bodySkin")
        cmds.setAttr(blendshape + ".Smile", 0.25)
        cmds.select(root, replace=True)

        subject = module._capture_subject()
        transforms, curves = module._sample_pose(subject)

        self.assertEqual(["root", "spine"], [bone["name"] for bone in subject["bones"]])
        self.assertEqual([1.0, 3.0, 2.0], transforms[0][:3])
        self.assertEqual([0.0, 0.0, 3.0], transforms[1][:3])
        self.assertEqual(["Smile"], [curve["name"] for curve in subject["curves"]])
        self.assertEqual([0.25], curves)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it under repository Python and verify the host test skips**

Run:

```powershell
python -m unittest discover -s maya/tools/MtoULiveLink/tests -p "test_maya_host.py" -v
```

Expected: one skipped test because system Python has no Maya module.

- [ ] **Step 3: Implement capture and sampling in the runtime file**

Use the selected joint only, walk joint children recursively, and derive every local child matrix from evaluated world matrices:

```python
import re


def _require_maya():
    if cmds is None or om is None:
        raise RuntimeError("MtoU_LiveLink must be run inside Maya 2022.4")


def _dag_path(path):
    selection = om.MSelectionList()
    selection.add(path)
    return selection.getDagPath(0)


def _maya_children(path):
    return cmds.listRelatives(path, children=True, fullPath=True, type="joint") or []


def _selected_root():
    selected = cmds.ls(selection=True, long=True) or []
    if len(selected) != 1 or cmds.nodeType(selected[0]) != "joint":
        raise ValueError("Select exactly one deformation root joint before connecting.")
    return selected[0]


def _discover_curve_plugs(bone_paths):
    skin_clusters = set()
    for path in bone_paths:
        skin_clusters.update(cmds.listConnections(path, type="skinCluster") or [])
    deformers = set()
    for skin_cluster in skin_clusters:
        for geometry in cmds.skinCluster(skin_cluster, query=True, geometry=True) or []:
            deformers.update(node for node in cmds.listHistory(geometry, pruneDagObjects=True) or []
                             if cmds.nodeType(node) == "blendShape")
    curves = []
    for deformer in sorted(deformers):
        aliases = cmds.aliasAttr(deformer, query=True) or []
        for alias, attribute in zip(aliases[0::2], aliases[1::2]):
            match = re.match(r"(?:weight|w)\[(\d+)\]$", attribute)
            if match:
                curves.append({"name": alias, "plug": deformer + "." + attribute,
                               "sort": (deformer, int(match.group(1)))})
    curves.sort(key=lambda curve: curve["sort"])
    grouped = []
    by_name = {}
    for curve in curves:
        group = by_name.get(curve["name"])
        if group is None:
            group = {"name": curve["name"], "plugs": [],
                     "sort": curve["sort"]}
            by_name[curve["name"]] = group
            grouped.append(group)
        group["plugs"].append(curve["plug"])
    return grouped


def _capture_subject():
    _require_maya()
    root = _selected_root()
    bones = build_hierarchy(root, _maya_children)
    for bone in bones:
        bone["dag_path"] = _dag_path(bone["path"])
    return {"root": root, "bones": bones,
            "curves": _discover_curve_plugs([bone["path"] for bone in bones]),
            "unit_scale": centimeters_per_unit(cmds.currentUnit(query=True, linear=True))}


def _sample_matrix(matrix, unit_scale):
    transform = om.MTransformationMatrix(matrix)
    translation = transform.translation(om.MSpace.kTransform)
    rotation = transform.rotation(asQuaternion=True)
    scale = transform.scale(om.MSpace.kTransform)
    return convert_transform(translation, rotation, scale, unit_scale)


def _sample_pose(subject):
    world_matrices = [bone["dag_path"].inclusiveMatrix() for bone in subject["bones"]]
    transforms = []
    for index, bone in enumerate(subject["bones"]):
        matrix = world_matrices[index]
        if bone["parent"] >= 0:
            matrix = matrix * world_matrices[bone["parent"]].inverse()
        transforms.append(_sample_matrix(matrix, subject["unit_scale"]))
    curves = []
    for curve in subject["curves"]:
        values = [float(cmds.getAttr(plug)) for plug in curve["plugs"]]
        reference = values[0]
        if any(abs(value - reference) > 1.0e-6 for value in values[1:]):
            raise ValueError("conflicting values for BlendShape alias {0}".format(
                curve["name"]))
        curves.append(reference)
    make_frame_message(transforms, curves)
    return transforms, curves
```

The `child_world * parent_world.inverse()` multiplication order is Maya's evaluated local transform and includes `jointOrient`, constraints, IK, controllers, and upstream groups without transmitting those nodes. Same-named aliases on separate skinned mesh parts are grouped to match FBX/Unreal Morph Target identity; disagreeing values are rejected instead of being chosen arbitrarily.

- [ ] **Step 4: Run the host test under Maya 2022 mayapy**

Run:

```powershell
$maya_root = (Get-ItemProperty 'HKLM:\SOFTWARE\Autodesk\Maya\2022\Setup\InstallPath').MAYA_INSTALL_LOCATION
& "$maya_root\bin\mayapy.exe" -m unittest discover -s maya/tools/MtoULiveLink/tests -v
```

Expected: all pure and Maya-host tests PASS; Maya reports API `20220400` if printed for diagnosis.

- [ ] **Step 5: Commit evaluated Maya sampling**

```powershell
git add maya/tools/MtoULiveLink/scripts/MtoULiveLink.py maya/tools/MtoULiveLink/tests
git commit -m "feat(maya): sample evaluated MtoU character data"
```

---

### Task 3: Maya sender worker, lifecycle, and native UI

**Files:**
- Modify: `maya/tools/MtoULiveLink/scripts/MtoULiveLink.py`
- Modify: `maya/tools/MtoULiveLink/tests/test_mtou_livelink.py`

**Interfaces:**
- Consumes: Task 2's captured subject and sampled pose.
- Produces: `_LatestFrame.put(packet)`, `_SenderWorker(init_message)`, `_Controller.connect()`, `_Controller.disconnect()`, and public `run()`.

- [ ] **Step 1: Add a deterministic newest-frame-wins test**

```python
class LatestFrameTests(unittest.TestCase):
    def test_only_newest_unsent_frame_is_kept(self):
        slot = MODULE._LatestFrame()
        slot.put(b"first")
        slot.put(b"second")
        self.assertEqual(b"second", slot.take())
        self.assertIsNone(slot.take())
```

Run the one test and expect FAIL because `_LatestFrame` is undefined.

- [ ] **Step 2: Implement the one-slot handoff and sender thread**

Add `select`, `threading`, and `time` imports. `_SenderWorker.run()` must perform this exact sequence:

```python
class _LatestFrame(object):
    def __init__(self):
        self._lock = threading.Lock()
        self._value = None

    def put(self, value):
        with self._lock:
            self._value = value

    def take(self):
        with self._lock:
            value, self._value = self._value, None
            return value


class _SenderWorker(threading.Thread):
    def __init__(self, init_message):
        threading.Thread.__init__(self, name="MtoULiveLinkSender")
        self.daemon = True
        self._init_packet = encode_message(init_message)
        self._latest = _LatestFrame()
        self._stop_event = threading.Event()
        self._lock = threading.Lock()
        self._socket = None
        self._state = "connecting"
        self._detail = "Connecting to Unreal..."
        self._missing_curves = []

    def submit(self, frame_message):
        self._latest.put(encode_message(frame_message))

    def status(self):
        with self._lock:
            return self._state, self._detail, list(self._missing_curves)

    def _set_status(self, state, detail, missing_curves=None):
        with self._lock:
            self._state = state
            self._detail = detail
            self._missing_curves = list(missing_curves or [])

    def stop(self):
        self._stop_event.set()
        with self._lock:
            sock = self._socket
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def run(self):
        try:
            sock = socket.create_connection((HOST, PORT), timeout=2.0)
            sock.settimeout(5.0)
            with self._lock:
                self._socket = sock
            sock.sendall(self._init_packet)
            reply = recv_message(sock)
            if reply.get("type") == "error":
                raise RuntimeError(reply.get("message") or "Unreal rejected the connection")
            if reply.get("type") != "ready":
                raise RuntimeError("expected ready, received {0}".format(reply.get("type")))
            self._set_status("ready", "Connected", reply.get("missing_curves") or [])
            sock.settimeout(0.25)
            while not self._stop_event.is_set():
                packet = self._latest.take()
                if packet is not None:
                    sock.sendall(packet)
                readable, _, _ = select.select([sock], [], [], 0.01)
                if readable and not sock.recv(1):
                    raise EOFError("Unreal closed the connection")
        except Exception as exc:
            if not self._stop_event.is_set():
                self._set_status("error", str(exc))
        finally:
            with self._lock:
                sock, self._socket = self._socket, None
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
            if self._stop_event.is_set():
                self._set_status("disconnected", "Disconnected")
```

Do not query `cmds`, `OpenMaya`, DAG paths, or attributes from this class.

- [ ] **Step 3: Implement one controller and Maya lifecycle cleanup**

`_Controller.connect()` captures the current root, creates the `init` records, starts `_SenderWorker`, registers exactly one timer and four lifecycle callbacks, and lets the timer poll status. Use these callbacks:

```python
self._callback_ids = [
    om.MSceneMessage.addCallback(om.MSceneMessage.kBeforeNew, self._on_scene_change),
    om.MSceneMessage.addCallback(om.MSceneMessage.kBeforeOpen, self._on_scene_change),
    om.MSceneMessage.addCallback(om.MSceneMessage.kMayaExiting, self._on_scene_change),
    om.MNodeMessage.addNodeDestroyedCallback(subject["bones"][0]["dag_path"].node(),
                                             self._on_root_destroyed),
]
self._timer_id = om.MTimerMessage.addTimerCallback(1.0 / 30.0, self._on_timer)
```

`_on_timer(elapsed, last_time, client_data)` must:

1. Read the worker status.
2. On first `ready`, display missing Morph Targets as a non-fatal status and submit the current pose immediately.
3. While `ready`, call `_sample_pose`, create one `frame`, and replace the worker's pending packet.
4. On `error`, copy the actionable message to the UI and call `disconnect(keep_error=True)`.

`disconnect()` must first remove the timer and every `MMessage` callback, then stop and join the worker for at most one second, clear subject state, and leave no thread or callback. It must be safe when called repeatedly and from scene/root/window callbacks.

- [ ] **Step 4: Build the minimal native Maya window and public entry point**

Use one `cmds.window` with five controls: root text, Connect button, Disconnect button, status text, and error text. Connect captures the current selection; do not add a separate selection button or host/port fields.

```python
_CONTROLLER = None
WINDOW_NAME = "MtoULiveLinkWindow"


def run():
    global _CONTROLLER
    _require_maya()
    if cmds.window(WINDOW_NAME, exists=True):
        cmds.showWindow(WINDOW_NAME)
        cmds.window(WINDOW_NAME, edit=True, restoreCommand="")
        return _CONTROLLER
    _CONTROLLER = _Controller()
    _CONTROLLER.build_ui()
    return _CONTROLLER


if __name__ == "__main__":
    run()
```

Set the window's `closeCommand` to `_Controller.close`, and make a repeated `run()` focus the existing window rather than adding callbacks.

- [ ] **Step 5: Run the Maya tests and a Script Editor smoke test**

Run:

```powershell
python -m unittest discover -s maya/tools/MtoULiveLink/tests -v
$maya_root = (Get-ItemProperty 'HKLM:\SOFTWARE\Autodesk\Maya\2022\Setup\InstallPath').MAYA_INSTALL_LOCATION
& "$maya_root\bin\mayapy.exe" -m unittest discover -s maya/tools/MtoULiveLink/tests -v
```

Expected: all tests PASS. In Maya 2022.4, execute `MtoULiveLink.py` twice and confirm one window exists; Connect without one selected joint reports the exact selection error and leaves no worker/callback.

- [ ] **Step 6: Commit the complete Maya runtime behavior**

```powershell
git add maya/tools/MtoULiveLink/scripts/MtoULiveLink.py maya/tools/MtoULiveLink/tests
git commit -m "feat(maya): stream evaluated MtoU poses"
```

---

### Task 4: Unreal plugin scaffold and protocol core

**Files:**
- Create from template: `unreal/Plugins/MtoULiveLink/`
- Modify: `unreal/Plugins/MtoULiveLink/MtoULiveLink.uplugin`
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/MtoULiveLink.Build.cs`
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkProtocol.h`
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkProtocol.cpp`
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkModule.cpp`
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/Tests/MtoULiveLinkTests.cpp`
- Modify: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor/MtoULiveLinkEditor.Build.cs`
- Modify: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor/Private/MtoULiveLinkEditorModule.cpp`
- Delete: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor/Public/MtoULiveLinkEditorModule.h`
- Modify: `unreal/ToolsLab.uproject`

**Interfaces:**
- Consumes: the exact protocol contract above and UE 5.7.4 headers.
- Produces: `FMtoUFrameDecoder::Append/Pop`, `FMtoUProtocol::ParseInit`, `ParseFrame`, `ValidateFrame`, `CompareSkeletons`, `EncodeReady`, `EncodeError`, `MakeStaticData`, and `MakeFrameData`.

- [ ] **Step 1: Reuse the Unreal scaffold and enable it in ToolsLab**

Run:

```powershell
python tools/create_project.py unreal-plugin mto-u-live-link --json
python tools/create_project.py unreal-plugin mto-u-live-link --apply --json
```

Replace the descriptor with version `0.1.0`, `FriendlyName` `MtoU_LiveLink`, `CanContainContent: false`, a Runtime `MtoULiveLink` module and Editor `MtoULiveLinkEditor` module, each with `PlatformAllowList: ["Win64"]` and `TargetAllowList: ["Editor"]`, plus this plugin dependency:

```json
"Plugins": [{"Name":"LiveLink","Enabled":true}]
```

Enable `MtoULiveLink` in `ToolsLab.uproject`:

```json
"Plugins": [{"Name":"MtoULiveLink","Enabled":true}]
```

- [ ] **Step 2: Add module rules using only required UE modules**

Runtime dependencies:

```csharp
using UnrealBuildTool;

public class MtoULiveLink : ModuleRules
{
    public MtoULiveLink(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Json", "LiveLinkAnimationCore", "LiveLinkInterface", "Sockets"
        });
    }
}
```

Editor dependencies:

```csharp
PrivateDependencyModuleNames.AddRange(new[]
{
    "AssetRegistry", "Core", "CoreUObject", "Engine", "MtoULiveLink", "UnrealEd"
});
```

Use `IMPLEMENT_MODULE(FDefaultModuleImpl, MtoULiveLink)` and `IMPLEMENT_MODULE(FDefaultModuleImpl, MtoULiveLinkEditor)` until Task 6 replaces the Runtime module implementation. Keep no public Editor module header.

- [ ] **Step 3: Write failing protocol automation tests**

Create tests named under `MtoULiveLink.Protocol` that assert:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUFramingTest,
    "MtoULiveLink.Protocol.PartialAndMultiplePackets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUInitValidationTest,
    "MtoULiveLink.Protocol.InitValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUFrameValidationTest,
    "MtoULiveLink.Protocol.FrameValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMtoUSkeletonComparisonTest,
    "MtoULiveLink.Protocol.OrderIndependentSkeletonComparison",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
```

The tests must feed a packet in 1/3/rest chunks, append two complete packets together, accept 701 parent-first bones, reject version `2`, reject transform/curve count mismatches, reject `1e400` as non-finite without marking the connection structural, and compare shuffled bone records by name/parent name. Diagnostics must contain separate `Missing in Unreal`, `Extra in Unreal`, and `Parent mismatches` sections.

- [ ] **Step 4: Implement the protocol types and incremental decoder**

Use these stable internal types:

```cpp
struct FMtoUBone
{
    FName Name;
    int32 ParentIndex = INDEX_NONE;
};

struct FMtoUTransform
{
    FVector Translation = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    FVector Scale = FVector::OneVector;
};

struct FMtoUInitMessage
{
    TArray<FMtoUBone> Bones;
    TArray<FName> Curves;
};

struct FMtoUFrameMessage
{
    TArray<FMtoUTransform> Transforms;
    TArray<double> Curves;
};

enum class EMtoUDecodeResult { NeedMore, Message, Error };

class FMtoUFrameDecoder
{
public:
    void Append(const uint8* Data, int32 Num);
    EMtoUDecodeResult Pop(TArray<uint8>& OutPayload, FString& OutError);
private:
    TArray<uint8> Buffer;
};
```

Decode the prefix with eight left shifts. If the `uint64` payload length exceeds `MAX_int32`, return `Error` because `TArray` cannot represent it; otherwise wait until the complete payload exists, remove exactly one packet, and leave later bytes buffered.

Parse UTF-8 with `FUTF8ToTCHAR`, parse JSON manually with `FJsonSerializer`, and require exact field types. Validate one root, parent-before-child indices, unique non-empty names, ten finite transform numbers, normalized non-zero quaternions, and exact counts. Do not reserve or reject by a product limit.

- [ ] **Step 5: Implement hierarchy diagnostics and native Live Link data builders**

`CompareSkeletons(const TArray<FMtoUBone>& Maya, const TArray<FMtoUBone>& Unreal)` must convert each array to `TMap<FName, FName>`, compare names and parent names, sort every diagnostic list, and return one deterministic `FString`.

Construct native data using the UE 5.7.4 interfaces verified in the installed engine:

```cpp
FLiveLinkStaticDataStruct StaticData(FLiveLinkSkeletonStaticData::StaticStruct());
FLiveLinkSkeletonStaticData* Skeleton = StaticData.Cast<FLiveLinkSkeletonStaticData>();
Skeleton->BoneNames = BoneNames;
Skeleton->BoneParents = BoneParents;
Skeleton->PropertyNames = AcceptedCurveNames;

FLiveLinkFrameDataStruct FrameData(FLiveLinkAnimationFrameData::StaticStruct());
FLiveLinkAnimationFrameData* Animation = FrameData.Cast<FLiveLinkAnimationFrameData>();
Animation->Transforms = Transforms;
Animation->PropertyValues = AcceptedCurveValues;
Animation->WorldTime = FLiveLinkWorldTime();
```

- [ ] **Step 6: Build and run the protocol tests**

Run:

```powershell
$ue_root = (Get-ItemProperty 'HKLM:\SOFTWARE\EpicGames\Unreal Engine\5.7').InstalledDirectory
& "$ue_root\Engine\Build\BatchFiles\Build.bat" UnrealEditor Win64 Development "$PWD\unreal\ToolsLab.uproject" -WaitMutex -NoHotReloadFromIDE
& "$ue_root\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\unreal\ToolsLab.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests MtoULiveLink.Protocol;Quit" -TestExit="Automation Test Queue Empty"
```

Expected: both modules compile without a new warning and every `MtoULiveLink.Protocol` test passes.

- [ ] **Step 7: Commit the Unreal protocol core**

```powershell
git add unreal/Plugins/MtoULiveLink unreal/ToolsLab.uproject
git commit -m "feat(unreal): add MtoU Live Link protocol core"
```

---

### Task 5: Unreal binding asset and placed actor

**Files:**
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Public/MtoULiveLinkBinding.h`
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Public/MtoULiveLinkActor.h`
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkActor.cpp`
- Modify: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/Tests/MtoULiveLinkTests.cpp`

**Interfaces:**
- Consumes: UE `UDataAsset`, `USkeletalMeshComponent`, and `ULiveLinkInstance`.
- Produces: `UMtoULiveLinkBinding::SkeletalMesh`, `AMtoULiveLinkActor::SetBinding`, `RefreshBinding`, `SetConnectionStatus`, and `GetSkeletalMeshComponent`.

- [ ] **Step 1: Add the binding and actor declarations**

```cpp
UCLASS(BlueprintType, meta=(DisplayName="MtoU_LiveLink Binding"))
class MTOULIVELINK_API UMtoULiveLinkBinding : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category="MtoU_LiveLink")
    TObjectPtr<USkeletalMesh> SkeletalMesh;
};
```

```cpp
UCLASS()
class MTOULIVELINK_API AMtoULiveLinkActor : public AActor
{
    GENERATED_BODY()
public:
    AMtoULiveLinkActor();
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void PostRegisterAllComponents() override;
    void SetBinding(UMtoULiveLinkBinding* InBinding);
    void SetConnectionStatus(const FString& InStatus);
    USkeletalMeshComponent* GetSkeletalMeshComponent() const { return SkeletalMeshComponent; }
    UMtoULiveLinkBinding* GetBinding() const { return Binding; }
private:
    void RefreshBinding();
    UPROPERTY(VisibleAnywhere, Category="MtoU_LiveLink")
    TObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent;
    UPROPERTY(VisibleAnywhere, Category="MtoU_LiveLink")
    TObjectPtr<UMtoULiveLinkBinding> Binding;
    UPROPERTY(VisibleAnywhere, Transient, Category="MtoU_LiveLink")
    FString ConnectionStatus = TEXT("Disconnected");
};
```

- [ ] **Step 2: Implement binding application without touching Actor transform**

The constructor creates one Skeletal Mesh Component as root. `RefreshBinding()` does only this:

```cpp
SkeletalMeshComponent->SetSkeletalMeshAsset(Binding ? Binding->SkeletalMesh : nullptr);
SkeletalMeshComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
SkeletalMeshComponent->SetAnimInstanceClass(ULiveLinkInstance::StaticClass());
if (ULiveLinkInstance* Instance = Cast<ULiveLinkInstance>(SkeletalMeshComponent->GetAnimInstance()))
{
    Instance->SetSubject(FLiveLinkSubjectName(FName(TEXT("MtoU_Character"))));
    Instance->EnableLiveLinkEvaluation(true);
}
```

Call it from `SetBinding`, `OnConstruction`, and `PostRegisterAllComponents`. Never call `SetActorTransform`, `SetWorldTransform`, or change component relative/world transform.

- [ ] **Step 3: Add the world-offset automation check**

Create an `EditorPreview` `UWorld`, spawn `AMtoULiveLinkActor`, place it at a non-zero transform, construct frame data whose root transform has non-zero translation, and assert both:

```cpp
TestTrue(TEXT("Root motion stays in Live Link frame"),
    AnimationData->Transforms[0].Equals(StreamedRoot));
TestTrue(TEXT("Actor placement is unchanged"),
    Actor->GetActorTransform().Equals(PlacedTransform));
```

Destroy the transient world at the end of the test.

- [ ] **Step 4: Build and run Runtime tests**

Run the Task 4 build command, then `Automation RunTests MtoULiveLink.Protocol;Quit`.

Expected: compile and tests PASS; no actor transform changes occur.

- [ ] **Step 5: Commit the binding and actor**

```powershell
git add unreal/Plugins/MtoULiveLink/Source/MtoULiveLink
git commit -m "feat(unreal): add MtoU binding actor"
```

---

### Task 6: Unreal listener, hierarchy gate, and Live Link source

**Files:**
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkSource.h`
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkSource.cpp`
- Modify: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/MtoULiveLinkModule.cpp`
- Modify: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLink/Private/Tests/MtoULiveLinkTests.cpp`

**Interfaces:**
- Consumes: Task 4 protocol builders and Task 5 placed actors/bindings.
- Produces: `FMtoULiveLinkSource : ILiveLinkSource, FRunnable`, an automatically registered source, one accepted client, `ready/error` responses, and publication of `MtoU_Character`.

- [ ] **Step 1: Declare the source's exact ownership and thread boundary**

```cpp
class FMtoULiveLinkSource final : public ILiveLinkSource,
                                  public FRunnable,
                                  public TSharedFromThis<FMtoULiveLinkSource>
{
public:
    explicit FMtoULiveLinkSource(uint16 InPort = 54321);
    virtual ~FMtoULiveLinkSource() override;
    virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid) override;
    virtual void Update() override;
    virtual bool IsSourceStillValid() const override;
    virtual bool RequestSourceShutdown() override;
    virtual FText GetSourceType() const override;
    virtual FText GetSourceMachineName() const override;
    virtual FText GetSourceStatus() const override;
    virtual uint32 Run() override;
    virtual void Stop() override;
    bool StartListener();
    void StopListener();
private:
    void HandleInitOnGameThread(FMtoUInitMessage&& Message);
    void PublishLatestFrameOnGameThread();
};
```

The worker exclusively owns `FSocket* ListenSocket` and `FSocket* ClientSocket`. It parses bytes and replaces one pending typed frame under `FCriticalSection`. The game thread exclusively scans UObjects, reads Skeletal Mesh assets, calls `ILiveLinkClient`, updates Actor status, and enqueues serialized replies into `TQueue<FMtoUOutgoing, EQueueMode::Spsc>`. No worker code may touch a UObject.

- [ ] **Step 2: Implement loopback-only socket lifecycle**

Create the address through `ISocketSubsystem`, set IP to `127.0.0.1`, set the configured port, create `NAME_Stream`, require exclusive port ownership without address reuse, bind, listen with backlog `1`, and set non-blocking. The worker loop must:

1. Accept a client when none exists.
2. While one exists, accept any second pending socket only to send `{"type":"error","message":"MtoU_LiveLink already has a Maya client."}` and close it.
3. Receive into a fixed 64 KiB temporary buffer; dynamic message storage remains in `FMtoUFrameDecoder`.
4. Allow exactly one `init` before `ready`, then only `frame` messages.
5. Drain game-thread replies; close after sending an `error` reply.
6. Sleep no more than 5 ms when no socket did work.
7. On `Stop`, leave the loop, close both sockets on the worker, and return; `StopListener` joins the thread and resets it.

Port `0` is allowed only for automation tests; production always uses `54321`.

- [ ] **Step 3: Validate every placed binding on the game thread**

At `init`, perform one connect-time `TObjectIterator<AMtoULiveLinkActor>` scan, filtering CDOs, invalid objects, and worlds other than `Editor`/`PIE`.

```cpp
// ponytail: one editor-world object scan per connection; add actor registration only if profiling shows this scan matters.
```

Reject with an actionable `error` if there is no placed actor, an actor has no binding, or a binding has no Skeletal Mesh. Convert each mesh's `FReferenceSkeleton` to `FMtoUBone` records and run `CompareSkeletons`; concatenate actor names and deterministic differences. Validate every placed actor so different hierarchies can never be reported ready.

Build accepted curve indices by retaining a Maya curve only when `SkeletalMesh->FindMorphTarget(CurveName)` succeeds for every participating mesh. Return all omitted names in `ready.missing_curves`, but do not reject a matching skeleton.

- [ ] **Step 4: Publish static and frame data through the native client**

On successful init:

```cpp
SubjectKey = FLiveLinkSubjectKey(SourceGuid, FName(TEXT("MtoU_Character")));
Client->PushSubjectStaticData_AnyThread(
    SubjectKey, ULiveLinkAnimationRole::StaticClass(),
    FMtoUProtocol::MakeStaticData(Init.Bones, AcceptedCurveNames));
```

Store expected incoming bone/curve counts and accepted curve indices before enqueuing `ready`. Each `Update()` moves the newest pending frame, filters its curves by those indices, and calls `PushSubjectFrameData_AnyThread`. On disconnect, retain the last Live Link subject/frame so placed actors keep their last pose; update transient Actor status to `Disconnected` and listen for a manual reconnect.

- [ ] **Step 5: Replace the default Runtime module with idempotent registration**

In `StartupModule`, first require the declared plugin dependency with `ensureMsgf(FModuleManager::Get().LoadModule("LiveLink"), TEXT("MtoULiveLink depends on the LiveLink module."))`, then retrieve the `ILiveLinkClient` modular feature, create one shared source, and call `AddSource`. In `ShutdownModule`, call `StopListener`, remove the source if the client feature still exists, and reset the shared pointer. `RequestSourceShutdown` repeats the same stop safely and returns `true` only after the worker has joined.

- [ ] **Step 6: Add source/socket automation coverage**

Add tests that:

- start `FMtoULiveLinkSource(0)`, call `StopListener()` twice, and verify no worker/socket remains;
- feed partial and combined packets through a real loopback connection;
- verify a second client receives the explicit rejection;
- verify structural count mismatch closes the session;
- verify a non-finite frame is dropped and a following valid frame remains available;
- verify source status reports a bind error instead of disappearing.

Use local sockets only and no sleeps longer than 50 ms; poll with a one-second test deadline.

- [ ] **Step 7: Build, test, and inspect Live Link source status**

Run the stock UE build and all `MtoULiveLink` automation tests. Then open ToolsLab, open the Live Link panel, and verify one `MtoU_LiveLink` source reports `Listening on 127.0.0.1:54321`. Start a second ToolsLab instance only for the port-conflict check; the second instance must retain a visible source with an actionable bind error.

- [ ] **Step 8: Commit the Runtime bridge**

```powershell
git add unreal/Plugins/MtoULiveLink/Source/MtoULiveLink
git commit -m "feat(unreal): publish MtoU Live Link frames"
```

---

### Task 7: Binding creation and drag-to-level factories

**Files:**
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor/Private/MtoULiveLinkFactories.h`
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor/Private/MtoULiveLinkFactories.cpp`
- Create: `unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor/Private/Tests/MtoULiveLinkEditorTests.cpp`

**Interfaces:**
- Consumes: `UMtoULiveLinkBinding` and `AMtoULiveLinkActor`.
- Produces: auto-discovered `UMtoULiveLinkBindingFactory` and `UMtoULiveLinkActorFactory`.

- [ ] **Step 1: Implement the new-asset factory**

```cpp
UCLASS(hidecategories=Object)
class UMtoULiveLinkBindingFactory : public UFactory
{
    GENERATED_BODY()
public:
    UMtoULiveLinkBindingFactory();
    virtual FText GetDisplayName() const override;
    virtual UObject* FactoryCreateNew(UClass* Class, UObject* Parent, FName Name,
        EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
};
```

Set `bCreateNew = true`, `bEditAfterNew = true`, `SupportedClass = UMtoULiveLinkBinding::StaticClass()`, return display name `MtoU_LiveLink Binding`, and create the asset with `RF_Transactional`. `UDataAsset` supplies the generic details editor; do not add a custom asset editor or Asset Definition.

- [ ] **Step 2: Implement the binding actor factory**

```cpp
UCLASS()
class UMtoULiveLinkActorFactory : public UActorFactory
{
    GENERATED_BODY()
public:
    UMtoULiveLinkActorFactory();
    virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
    virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
    virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
};
```

Set `DisplayName` to `MtoU_LiveLink Actor`, `NewActorClass` to `AMtoULiveLinkActor`, and reject a binding without a Skeletal Mesh using `Select a Skeletal Mesh on the binding before placing it.` In `PostSpawnActor`, cast both arguments and call `SetBinding`; `GetAssetFromActorInstance` returns the actor's binding.

- [ ] **Step 3: Add Editor factory tests**

Instantiate `UMtoULiveLinkBindingFactory`, assert `FactoryCreateNew` returns a transactional `UMtoULiveLinkBinding`, assert the Actor factory rejects that empty binding with the exact error, assign a transient `USkeletalMesh`, and assert it becomes placeable and spawns `AMtoULiveLinkActor` with the same binding.

- [ ] **Step 4: Build, run tests, and perform the Content Browser smoke test**

Expected in ToolsLab:

1. `MtoU_LiveLink Binding` appears in the new-asset menu.
2. Double-click opens generic details with one Skeletal Mesh field.
3. Dragging an empty binding is refused with the exact message.
4. Dragging a configured binding creates `AMtoULiveLinkActor` and preserves the drop transform.

- [ ] **Step 5: Commit the Editor workflow**

```powershell
git add unreal/Plugins/MtoULiveLink/Source/MtoULiveLinkEditor
git commit -m "feat(unreal): add MtoU binding workflow"
```

---

### Task 8: Bilingual docs, changelogs, index, and packaging checks

**Files:**
- Modify: `maya/tools/MtoULiveLink/README.md`
- Modify: `maya/tools/MtoULiveLink/README_CN.md`
- Modify: `maya/tools/MtoULiveLink/CHANGELOG.md`
- Modify: `unreal/Plugins/MtoULiveLink/README.md`
- Modify: `unreal/Plugins/MtoULiveLink/README_CN.md`
- Modify: `unreal/Plugins/MtoULiveLink/CHANGELOG.md`
- Modify: `README.md`
- Modify: `README_CN.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: completed user workflow from Tasks 3, 6, and 7.
- Produces: independently usable installation/usage docs and repository discovery links.

- [ ] **Step 1: Write the matching Maya README pair**

English content must say:

- Introduction: streams one evaluated Maya deformation skeleton and matching BlendShapes to the local Unreal `MtoU_Character` Live Link subject without editing or exporting the scene.
- Supported Versions: Windows 64-bit and Maya 2022.4 only.
- Installation: copy `scripts/MtoULiveLink.py` to a Maya scripts directory or open the file directly in Maya's Python Script Editor.
- Usage: place/configure the Unreal binding actor first; select exactly one deformation root; execute the file; Connect; pose/play/scrub; Disconnect; reconnect after topology edits or either host restarts.

The Chinese README must carry the same facts, steps, limitations, and cross-link, with no development/protocol section.

- [ ] **Step 2: Write the matching Unreal README pair**

English content must say:

- Introduction: receives one local Maya character as native Live Link animation and curves while preserving each placed actor transform.
- Supported Versions: Windows 64-bit, stock Unreal Editor 5.7.4; third-party UE 5.7 compatibility is not yet claimed.
- Installation: copy the whole plugin under the project's `Plugins` directory, compile, enable Live Link and MtoU_LiveLink, then restart.
- Usage: create `MtoU_LiveLink Binding` in the chosen Content Browser folder, set the existing Skeletal Mesh, drag it into the level, then connect Maya; no Animation Sequence is created.

The Chinese README must match exactly in scope and limitations.

- [ ] **Step 3: Update changelogs and root indexes**

Each project changelog gets `## 0.1.0` with one initial release entry describing its side. Root English/Chinese tool tables add one Maya tool row and one Unreal plugin row, and both documentation lists link to the corresponding project README. Root `Unreleased` gets one line: `Add the MtoU_LiveLink Maya sender and Unreal Live Link receiver.`

- [ ] **Step 4: Run tests, repository validation, and dry-run packaging**

```powershell
python -m unittest discover -s maya/tools/MtoULiveLink/tests -v
python -m unittest discover -s tests -v
python tools/validate_repository.py
python tools/package_maya_tool.py MtoULiveLink --json
python tools/package_unreal_plugin.py MtoULiveLink --engine 5.7 --json
git status --short
```

Expected: tests and validator PASS; package commands report `dry-run`, version `0.1.0`, and non-zero file counts; status contains no generated directory, cache, or archive.

- [ ] **Step 5: Commit documentation and integration metadata**

```powershell
git add README.md README_CN.md CHANGELOG.md maya/tools/MtoULiveLink/README.md maya/tools/MtoULiveLink/README_CN.md maya/tools/MtoULiveLink/CHANGELOG.md unreal/Plugins/MtoULiveLink/README.md unreal/Plugins/MtoULiveLink/README_CN.md unreal/Plugins/MtoULiveLink/CHANGELOG.md
git commit -m "docs: document MtoU Live Link"
```

---

### Task 9: Stock-engine build and production acceptance gate

**Files:**
- Verify only; do not add generated files or machine-specific results to the repository.

**Interfaces:**
- Consumes: the complete Maya tool, Unreal plugin, existing FBX-imported Skeletal Mesh, and production Maya rig.
- Produces: evidence for the stock UE 5.7.4 support claim; no compatibility claim for the modified engine until repeated there.

- [x] **Step 1: Confirm exact host versions**

```powershell
$ue_root = (Get-ItemProperty 'HKLM:\SOFTWARE\EpicGames\Unreal Engine\5.7').InstalledDirectory
Get-Content -LiteralPath "$ue_root\Engine\Build\Build.version" | python -m json.tool
$maya_root = (Get-ItemProperty 'HKLM:\SOFTWARE\Autodesk\Maya\2022\Setup\InstallPath').MAYA_INSTALL_LOCATION
& "$maya_root\bin\mayapy.exe" --version
```

Expected: UE `5.7.4` build `51494982` and Python `3.7.7` from Maya 2022.4.

- [x] **Step 2: Run the complete automated verification chain**

```powershell
& "$maya_root\bin\mayapy.exe" -m unittest discover -s maya/tools/MtoULiveLink/tests -v
python -m unittest discover -s tests -v
python tools/validate_repository.py
& "$ue_root\Engine\Build\BatchFiles\Build.bat" UnrealEditor Win64 Development "$PWD\unreal\ToolsLab.uproject" -WaitMutex -NoHotReloadFromIDE
& "$ue_root\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\unreal\ToolsLab.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests MtoULiveLink;Quit" -TestExit="Automation Test Queue Empty"
```

Expected: every command exits `0`, both modules compile without a new warning, and all Maya/Unreal tests pass.

- [x] **Step 3: Run the end-to-end production-rig acceptance**

Use the unchanged FBX workflow to supply the existing Skeletal Mesh, place the binding actor at a non-zero transform, connect the referenced/proxy Maya rig, and record:

- hierarchy validation result for the correct mesh;
- explicit missing/extra/parent diagnostics for an intentionally wrong mesh;
- control-, IK-, constraint-, current-frame-key-, and BlendShape-driven visual updates;
- playback and scrubbing update rate, serialized frame size, and visible latency;
- at least 700 bones with visible update under 100 ms on the production workstation;
- unchanged Actor transform while streamed root-bone motion is visible;
- Disconnect/Connect, script rerun, scene New/Open, root deletion, editor exit, and plugin shutdown with no duplicate callback, thread, source, or socket;
- a second Maya client rejected cleanly;
- no Animation Sequence, binding mutation, Skeletal Mesh mutation, or other project-content write.

- [x] **Step 4: Record the modified UE 5.7 installation as out of scope**

Per the 2026-08-11 acceptance direction, do not validate or claim compatibility
with the third-party-modified UE 5.7 installation. Only stock Unreal Editor
5.7.4 is in scope. If that scope changes, rerun Steps 2–3 against the modified
installation before changing the README claim.

- [x] **Step 5: Final residue and documentation check**

```powershell
git status --short
git ls-files --others --exclude-standard
python tools/validate_repository.py
```

Expected: only intended source/documentation changes are visible; no Unreal generated folder, Maya cache, archive, user binding asset, or machine-specific absolute path is tracked. If verification exposed defects, fix them in the owning task and rerun the narrow test before this full chain; otherwise make no verification-only commit.

Outcome: completed on 2026-08-11. See
[`docs/mtou-livelink-stock-acceptance-2026-08-11.md`](../../mtou-livelink-stock-acceptance-2026-08-11.md)
for the stock-engine evidence and bounded warnings. The feature branch remains
unmerged pending the user's later integration decision.
