"""Fixture driven fake Unreal publisher for the camera sync prototype.

Two jobs:

* build the fixture JSON used by both the peer and this server (``build_fixture``),
  including the Unreal-side marker NDC computed from Unreal's own rules;
* replay that fixture over the real wire format on a real socket, so the Maya
  peer can be exercised without a running Unreal.

The Unreal-side projection here is deliberately not the Maya one: marker NDC is
derived from Unreal's constrained-FOV rules on the film plane in millimetres
(``unreal_marker_ndc``), while Maya projects with its film-aperture and film-fit
model. Agreement between them is what the prototype asserts.

    python mock_camera_sync_server.py --write-fixture fixture.json
    python mock_camera_sync_server.py --fixture fixture.json --host 127.0.0.1 --port 54331
"""

import argparse
import importlib.util
import json
import math
import socket
import sys
import threading
import time
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import mtou_camera_sync_mapping as mapping  # noqa: E402

PROTOCOL_NAME = mapping.PROTOCOL_NAME
PROTOCOL_VERSION = mapping.PROTOCOL_VERSION
DEFAULT_PORT = 54330

# Client messages the publisher accepts; anything else is refused, mirroring
# protocol.md's structural guarantee that Maya cannot control time.
CLIENT_TYPES = ("hello", "applied", "bye")


def load_fixture(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, value):
    data = json.dumps(value, indent=2, ensure_ascii=True)
    target = Path(path)
    target.write_text(data, encoding="utf-8")
    return target


# --------------------------------------------------------------------------
# Fixture authoring
# --------------------------------------------------------------------------

def ue_xy(vector):
    return mapping.ue_to_maya_point(vector[0], vector[1], vector[2])


def unreal_camera_space(point_ue, location_ue, right, up, forward):
    """Unreal camera space: X forward, Y right, Z up."""
    delta = (point_ue[0] - location_ue[0],
             point_ue[1] - location_ue[1],
             point_ue[2] - location_ue[2])
    return (sum(a * b for a, b in zip(delta, forward)),
            sum(a * b for a, b in zip(delta, right)),
            sum(a * b for a, b in zip(delta, up)))


def unreal_marker_ndc(point_ue, camera, resolution):
    """Unreal's marker NDC from its own constrained field of view rules.

    The film-plane coordinates come from the focal length and the sensor size in
    millimetres; which half extent the constraint fixes is Unreal's
    ``AspectRatioAxisConstraint`` behaviour, and the resolution aspect supplies
    the other axis.
    """
    location = camera["location"]
    right = camera["right"]
    up = camera["up"]
    forward = camera["forward"]
    local = unreal_camera_space(
        (point_ue[0], point_ue[1], point_ue[2]),
        (location["x"], location["y"], location["z"]),
        (right["x"], right["y"], right["z"]),
        (up["x"], up["y"], up["z"]),
        (forward["x"], forward["y"], forward["z"]))
    depth = local[0]
    if depth <= 0.0:
        return None
    focal = float(camera["focal_length_mm"])
    sensor_x = float(camera["sensor_width_mm"])
    sensor_y = float(camera["sensor_height_mm"])
    aspect = float(resolution[0]) / float(resolution[1])
    constraint = mapping.normalize_aspect_constraint(
        camera.get("aspect_axis_constraint"))
    if constraint == "maintain_yfov":
        half_y = sensor_y / 2.0
        half_x = half_y * aspect
    elif constraint == "maintain_major_axis_fov" and aspect < 1.0:
        half_y = sensor_y / 2.0
        half_x = half_y * aspect
    else:
        half_x = sensor_x / 2.0
        half_y = half_x / aspect
    u = focal * local[1] / depth
    v = focal * local[2] / depth
    offset_x = float(camera.get("sensor_horizontal_offset_mm") or 0.0)
    offset_y = float(camera.get("sensor_vertical_offset_mm") or 0.0)
    return [(u - offset_x) / half_x, (v - offset_y) / half_y]


def horizontal_fov_deg(sensor_width_mm, focal_length_mm):
    """Unreal's CurrentHorizontalFOV for a sensor width and focal length."""
    return math.degrees(math.atan2(sensor_width_mm / 2.0, focal_length_mm))


def build_camera(location_ue, rotation_deg, focal_length_mm, sensor_width_mm,
                 sensor_height_mm, constraint="MaintainXFOV", f_stop=2.8,
                 focus_distance_cm=500.0, depth_of_field=True, near_clip_cm=10.0,
                 sensor_horizontal_offset_mm=0.0, sensor_vertical_offset_mm=0.0,
                 squeeze_factor=1.0):
    """One Unreal camera payload, with both the basis and the rotation."""
    right, up, forward = mapping.ue_basis_vectors(rotation_deg)

    def vector(values):
        return {"x": values[0], "y": values[1], "z": values[2]}

    return {
        "name": "CineCameraActor",
        "actor": "CineCameraActor_1",
        "location": vector(location_ue),
        "rotation": {"roll": rotation_deg[0], "pitch": rotation_deg[1],
                     "yaw": rotation_deg[2]},
        "right": vector(right),
        "up": vector(up),
        "forward": vector(forward),
        "focal_length_mm": focal_length_mm,
        "horizontal_fov_deg": horizontal_fov_deg(sensor_width_mm, focal_length_mm),
        "sensor_width_mm": sensor_width_mm,
        "sensor_height_mm": sensor_height_mm,
        "sensor_aspect_ratio": sensor_width_mm / sensor_height_mm,
        "sensor_horizontal_offset_mm": sensor_horizontal_offset_mm,
        "sensor_vertical_offset_mm": sensor_vertical_offset_mm,
        "aspect_axis_constraint": constraint,
        "constrain_aspect_ratio": False,
        "f_stop": f_stop,
        "focus_method": "Manual" if depth_of_field else "Disable",
        "focus_distance_cm": focus_distance_cm,
        "depth_of_field": depth_of_field,
        "squeeze_factor": squeeze_factor,
        "near_clip_cm": near_clip_cm,
        "far_clip_cm": None,
    }


def build_fixture(port=DEFAULT_PORT, scene_fps=24.0, maya_start_frame=1001.0,
                  playback_start=1000.0, display_rate=(24, 1), resolution=(1920, 1080),
                  frames_to_apply=3, host="127.0.0.1", camera_name="MtoU_UE_Camera",
                  render=False, evidence_dir=None, marker_tolerance_ndc=0.002,
                  far_clip_fallback_cm=100000.0):
    """A complete fixture: session, frames and the samples the peer asserts.

    The representative payload uses a 2.39:1 style sensor on a 16:9 gate with
    film offsets, depth of field on, a non-zero playback range and a sub-frame
    time step, so every adaptation the prototype claims is exercised.
    """
    sensor_width_mm = 24.89
    sensor_height_mm = 10.41
    focal_length_mm = 35.0
    sequence_name = "MtoU_CameraSync"
    location = (1200.0, 300.0, 420.0)
    rotation = (0.0, -6.0, 32.0)
    camera = build_camera(
        location, rotation, focal_length_mm, sensor_width_mm, sensor_height_mm,
        sensor_horizontal_offset_mm=0.5, sensor_vertical_offset_mm=-0.25,
        f_stop=2.8, focus_distance_cm=850.0, depth_of_field=True, near_clip_cm=5.0)
    right, up, forward = mapping.ue_basis_vectors(rotation)
    # Marker actors placed in the world relative to the evaluated camera, so the
    # fixture frames them: (name, depth along forward, right offset, up offset).
    marker_layout = [("Marker_Centre", 3000.0, 0.0, 0.0),
                     ("Marker_Right", 2400.0, 500.0, 280.0),
                     ("Marker_Left_Low", 3600.0, -820.0, -430.0)]
    markers = []
    for name, depth, offset_right, offset_up in marker_layout:
        point = tuple(location[axis] + depth * forward[axis]
                      + offset_right * right[axis] + offset_up * up[axis]
                      for axis in range(3))
        marker = {"name": name, "location": {"x": point[0], "y": point[1], "z": point[2]}}
        ndc = unreal_marker_ndc(point, camera, resolution)
        marker["ndc"] = {"x": ndc[0], "y": ndc[1]}
        marker["inside"] = -1.0 <= ndc[0] <= 1.0 and -1.0 <= ndc[1] <= 1.0
        markers.append(marker)

    display_numerator, display_denominator = display_rate
    frames = []
    expected = []
    for index in range(frames_to_apply):
        # 0, 1 and a half display frames after the range start.
        display_frame = float(playback_start) + (0.0, 1.0, 1.5)[index % 3]
        frame_camera = json.loads(json.dumps(camera))
        frame_camera["focal_length_mm"] = focal_length_mm + index * 5.0
        frame_markers = []
        for marker in markers:
            entry = json.loads(json.dumps(marker))
            ndc = unreal_marker_ndc(
                (entry["location"]["x"], entry["location"]["y"], entry["location"]["z"]),
                frame_camera, resolution)
            entry["ndc"] = {"x": ndc[0], "y": ndc[1]}
            entry["inside"] = -1.0 <= ndc[0] <= 1.0 and -1.0 <= ndc[1] <= 1.0
            frame_markers.append(entry)
        frame = {
            "frame_serial": index + 1,
            "eval_serial": index + 1,
            # The documented shape: <sequence>@<tick>+<milli-tick>/<camera path>#<content>.
            "eval_identity": "{0}@{1}+000/{2}#{3}".format(
                sequence_name, int(round(display_frame * display_numerator)),
                "CineCameraActor", "fixture-camera-content"),
            "repeat": 2,
            "time": {"display_frame": display_frame,
                     "seconds": (display_frame - float(playback_start))
                     / (display_numerator / display_denominator),
                     "source_frame": int(display_frame),
                     "tick": int(display_frame * display_numerator),
                     "display_rate": {"numerator": display_numerator,
                                      "denominator": display_denominator}},
            "camera_cut": {"camera": "CineCameraActor", "stage": "root"},
            "output_resolution": {"x": resolution[0], "y": resolution[1]},
            "camera": frame_camera,
            "view": {"location": camera["location"], "rotation": camera["rotation"]},
            "projection": {"view_proj": [0.0] * 16,
                           "view_rect": {"x": 0, "y": 0, "width": resolution[0],
                                         "height": resolution[1]}},
            "markers": frame_markers,
        }
        frames.append(frame)
        maya_time, subframe = mapping.maya_time_for_frame(
            display_frame, playback_start, {"numerator": display_numerator,
                                            "denominator": display_denominator},
            scene_fps, maya_start_frame)
        attributes, notes = mapping.camera_parameters(
            frame_camera, {"far_clip_fallback_cm": far_clip_fallback_cm},
            resolution, frame)
        expected.append({
            "frame_serial": frame["frame_serial"],
            "unreal_display_frame": display_frame,
            "maya_time": maya_time,
            "maya_frame": mapping.applied_maya_time(maya_time, subframe),
            "subframe": subframe,
            "focal_length_mm": frame_camera["focal_length_mm"],
            "horizontal_film_aperture_in": attributes["horizontalFilmAperture"],
            "vertical_film_aperture_in": attributes["verticalFilmAperture"],
            "horizontal_film_offset_in": attributes["horizontalFilmOffset"],
            "vertical_film_offset_in": attributes["verticalFilmOffset"],
            "film_fit": attributes["filmFit"],
            "f_stop": attributes["fStop"],
            "focus_distance_cm": attributes["focusDistance"],
            "depth_of_field": attributes["depthOfField"],
            "near_clip_cm": attributes["nearClipPlane"],
            "far_clip_used_cm": attributes["farClipPlane"],
            "lens_squeeze_ratio": attributes["lensSqueezeRatio"],
            "max_marker_delta": 1e-9,
        })

    return {
        "protocol": {"name": PROTOCOL_NAME, "version": PROTOCOL_VERSION},
        "host": host,
        "port": port,
        "scene_fps": scene_fps,
        "maya_start_frame": maya_start_frame,
        "maya_origin_frame": maya_start_frame,
        "playback_range": {"start": float(playback_start), "end": float(playback_start) + 100.0},
        "camera_name": camera_name,
        "frames_to_apply": frames_to_apply,
        "render": render,
        "evidence_dir": evidence_dir,
        "marker_tolerance_ndc": marker_tolerance_ndc,
        "send_interval": 0.005,
        "end_after_frames": False,
        "session": {
            "display_rate": {"numerator": display_numerator,
                             "denominator": display_denominator},
            "tick_resolution": {"numerator": 24000, "denominator": 1001},
            "output_resolution": {"x": resolution[0], "y": resolution[1]},
            "far_clip_fallback_cm": far_clip_fallback_cm,
            "camera_cut": {"camera": "CineCameraActor", "stage": "root"},
            "marker_names": [marker["name"] for marker in markers],
            "time_authority": "unreal",
            "sequence": sequence_name,
        },
        "frames": frames,
        "expected": expected,
    }


def session_message(fixture, port):
    message = dict(fixture["session"])
    message.update({
        "type": "session",
        "protocol": PROTOCOL_NAME,
        "version": PROTOCOL_VERSION,
        "port": port,
        "playback_range": fixture["playback_range"],
    })
    return message


def frame_message(fixture, frame):
    message = dict(frame)
    message["type"] = "frame"
    message["session"] = fixture["session"]["sequence"]
    message["sequence"] = 1
    return message


# --------------------------------------------------------------------------
# Fake publisher
# --------------------------------------------------------------------------

class MockPublisher(object):
    """Replays a fixture over a real TCP socket, one client at a time."""

    def __init__(self, fixture, host=None, port=None, send_interval=None,
                 log=None, client_timeout=60.0):
        self.fixture = fixture
        self.host = host or fixture.get("host") or "127.0.0.1"
        self.requested_port = port if port is not None else fixture.get("port", DEFAULT_PORT)
        self.send_interval = (fixture.get("send_interval", 0.005)
                              if send_interval is None else send_interval)
        self.client_timeout = float(client_timeout)
        self.port = None
        self.log = log or []
        self.hello = None
        self.applied = []
        self.refused = 0
        self.errors = []
        self._server = None
        self._thread = None
        self._client_thread = None
        self._client_connection = None
        self._active = False
        self._stop = threading.Event()

    # -- lifecycle ---------------------------------------------------------

    def start(self):
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind((self.host, int(self.requested_port)))
        self.port = self._server.getsockname()[1]
        self._server.listen(4)
        self._server.settimeout(0.2)
        self._thread = threading.Thread(target=self._serve_forever, name="MockPublisher")
        self._thread.daemon = True
        self._thread.start()
        return self.port

    def stop(self):
        self._stop.set()
        connection = self._client_connection
        if connection is not None:
            # Unblock the session thread's receive so a stopped publisher looks
            # like the lost connection it is.
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                connection.close()
            except OSError:
                pass
        if self._client_thread is not None:
            self._client_thread.join(timeout=10.0)
        if self._thread is not None:
            self._thread.join(timeout=10.0)
        if self._server is not None:
            try:
                self._server.close()
            except OSError:
                pass
        self._server = None

    # -- protocol ----------------------------------------------------------

    def _serve_forever(self):
        while not self._stop.is_set():
            if self._client_thread is not None and not self._client_thread.is_alive():
                if self.fixture.get("single_session", True):
                    # A one-shot replay: the publisher is done once its client left.
                    return
                self._client_thread = None
            try:
                connection, address = self._server.accept()
            except (socket.timeout, OSError):
                continue
            if self._active:
                # protocol.md: a second connection is refused, not queued. The
                # session runs in its own thread so the refusal is immediate.
                self.refused += 1
                try:
                    connection.sendall((json.dumps({
                        "type": "error",
                        "category": "CLIENT_ALREADY_CONNECTED",
                        "detail": "one client at a time"}) + "\n").encode("utf-8"))
                except OSError:
                    pass
                connection.close()
                continue
            self._active = True
            self._client_thread = threading.Thread(
                target=self._serve_client_guarded, args=(connection, address),
                name="MockPublisherClient")
            self._client_thread.daemon = True
            self._client_thread.start()

    def _serve_client_guarded(self, connection, address):
        self._client_connection = connection
        try:
            self._serve_client(connection, address)
        finally:
            self._active = False
            self._client_connection = None
            try:
                connection.close()
            except OSError:
                pass

    def _serve_client(self, connection, address):
        connection.settimeout(self.client_timeout)
        buffer = b""
        greeted = False
        deadline = time.time() + self.client_timeout
        while not self._stop.is_set() and time.time() < deadline:
            try:
                chunk = connection.recv(65536)
            except (socket.timeout, OSError):
                break
            if not chunk:
                break
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    message = json.loads(line.decode("utf-8"))
                except ValueError as error:
                    self.errors.append("undecodable client line: {0}".format(error))
                    continue
                kind = message.get("type")
                if kind not in CLIENT_TYPES:
                    self.errors.append("refused client message {0!r}".format(kind))
                    try:
                        connection.sendall((json.dumps({
                            "type": "error",
                            "category": "CLIENT_MAY_NOT_CONTROL_TIME",
                            "detail": "message type {0!r} is client-controlled "
                                      "nowhere in this protocol".format(kind)}) + "\n")
                            .encode("utf-8"))
                    except OSError:
                        pass
                    continue
                if kind == "hello":
                    self.hello = message
                    if not greeted:
                        greeted = True
                        self._send(connection, session_message(self.fixture, self.port))
                        self._send_frames(connection)
                    continue
                if kind == "applied":
                    self.applied.append(message)
                    continue
                if kind == "bye":
                    self.log.append("client left with bye")
                    return
            if greeted and self.fixture.get("end_after_frames") and not self._stop.is_set():
                if len(self.applied) >= self._expected_applied():
                    self._send(connection, {"type": "end", "reason": "fixture complete"})
                    time.sleep(0.05)
                    return

    def _expected_applied(self):
        return sum(int(frame.get("repeat", 1)) for frame in self.fixture["frames"])

    def _send_frames(self, connection):
        for frame in self.fixture["frames"]:
            for _ in range(int(frame.get("repeat", 1))):
                if self._stop.is_set():
                    return
                self._send(connection, frame_message(self.fixture, frame))
                time.sleep(self.send_interval)

    def _send(self, connection, message):
        connection.sendall((json.dumps(message, ensure_ascii=True) + "\n").encode("utf-8"))


def serve(fixture, host=None, port=None, applied_log=None, ready_file=None,
          client_timeout=120.0, log=None):
    """Blocking serve used by the command line (and by tests in a thread).

    Returns once the single replay session finished or ``stop`` is called.
    """
    publisher = MockPublisher(fixture, host=host, port=port, client_timeout=client_timeout)
    publisher.start()
    if ready_file:
        write_json(ready_file, {"port": publisher.port})
    message = "mock publisher listening on {0}:{1}".format(publisher.host, publisher.port)
    print(message, flush=True)
    (log if log is not None else []).append(message)
    try:
        publisher._thread.join()
    except KeyboardInterrupt:
        pass
    finally:
        publisher.stop()
    if applied_log:
        write_json(applied_log, {"hello": publisher.hello, "applied": publisher.applied,
                                 "errors": publisher.errors, "refused": publisher.refused,
                                 "port": publisher.port})
    return publisher


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", default=None)
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--applied-log", default=None)
    parser.add_argument("--ready-file", default=None,
                        help="write the bound port here once listening")
    parser.add_argument("--write-fixture", default=None,
                        help="generate a fixture with the built in representative payload")
    parser.add_argument("--scene-fps", type=float, default=24.0)
    parser.add_argument("--maya-start-frame", type=float, default=1001.0)
    parser.add_argument("--frames", type=int, default=3)
    parser.add_argument("--render", action="store_true")
    parser.add_argument("--evidence-dir", default=None)
    args = parser.parse_args(argv)
    if args.write_fixture:
        fixture = build_fixture(scene_fps=args.scene_fps,
                                maya_start_frame=args.maya_start_frame,
                                frames_to_apply=args.frames, render=args.render,
                                evidence_dir=args.evidence_dir,
                                port=DEFAULT_PORT if args.port is None else args.port)
        write_json(args.write_fixture, fixture)
        print("wrote fixture {0}".format(args.write_fixture))
        if not args.fixture:
            return 0
    if not args.fixture:
        parser.error("--fixture is required unless only --write-fixture is used")
    fixture = load_fixture(args.fixture)
    if args.render:
        fixture["render"] = True
    if args.evidence_dir:
        fixture["evidence_dir"] = args.evidence_dir
    serve(fixture, host=args.host, port=args.port, applied_log=args.applied_log,
          ready_file=args.ready_file)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
