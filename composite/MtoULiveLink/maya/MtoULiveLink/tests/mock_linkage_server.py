# ruff: noqa: E402
"""Minimal Unreal-side peer for the linkage host checks.

Implements the v11 linkage conversation from the Unreal side (protocol/v11.md)
so the Maya adapter can be exercised without an editor: it greets the client,
accepts one object, sends camera/time targets, applies the answered groups,
reports the application evidence, and records latency and convergence.

    python mock_linkage_server.py --port 54321 --targets 40 --result out.json

The peer injects an optional round-trip delay (`--delay-ms`) and an optional
application delay (`--application-delay-ms`) so "advancing target must not
cancel the issued one" and "stop converges in bounded time" can be measured
rather than asserted by inspection. It never talks to a real Unreal Editor and
is not part of the product.
"""

import argparse
import json
import socket
import struct
import threading
import time

MAX_MESSAGE_BYTES = 32 * 1024 * 1024


def encode(message):
    payload = json.dumps(message, ensure_ascii=False, allow_nan=False,
                         separators=(",", ":")).encode("utf-8")
    return struct.pack(">Q", len(payload)) + payload


def recv_exact(sock, size):
    chunks = []
    while size:
        chunk = sock.recv(size)
        if not chunk:
            raise EOFError("peer closed the connection")
        chunks.append(chunk)
        size -= len(chunk)
    return b"".join(chunks)


def recv_message(sock):
    size = struct.unpack(">Q", recv_exact(sock, 8))[0]
    if size > MAX_MESSAGE_BYTES:
        raise ValueError("message exceeds the frozen framing ceiling")
    return json.loads(recv_exact(sock, size).decode("utf-8"))


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))
    return ordered[index]


class MockUnrealPeer(object):
    """One Unreal-side linkage conversation."""

    def __init__(self, port, targets=20, delay_ms=0.0, cadence_s=0.01,
                 application_delay_s=0.0, fps=24.0, pause_frames=3,
                 object_id="character", listen_timeout=120.0):
        self.port = int(port)
        self.targets = int(targets)
        self.delay = float(delay_ms) / 1000.0
        self.cadence = float(cadence_s)
        self.application_delay = float(application_delay_s)
        self.fps = float(fps)
        self.pause_frames = int(pause_frames)
        self.object_id = object_id
        self.listen_timeout = float(listen_timeout)
        self.session_id = 71
        self.set_revision = 1
        self.space_revision = 1
        self.mode_revision = 1
        self.transport_serial = 0
        self.frame_serial = 0
        self.eval_serial = 0
        self.issued = None
        self.pending = None
        self.applied = []
        self.latencies = []
        self.groups = []
        self.errors = []
        self.identity = {}
        self.mode = "realtime"
        self.leaving = None
        self.stop = threading.Event()
        self.last_target_time = None
        self.converged_at = None
        self.started = time.monotonic()

    # ------------------------------------------------------------------ wire
    def _send(self, sock, message):
        if self.delay:
            time.sleep(self.delay)
        sock.sendall(encode(message))

    def serve(self, sock):
        hello = recv_message(sock)
        self.identity["hello"] = hello
        if hello.get("type") != "hello":
            self.errors.append("first message was {0!r}".format(hello.get("type")))
            return
        if hello.get("version") != 11:
            self.errors.append("hello version {0!r}".format(hello.get("version")))
            return
        self._send(sock, {
            "type": "hello_ack",
            "version": 11,
            "session_id": self.session_id,
            "capabilities": {"linkage": True, "reference": True},
            "targets": [{"object_id": self.object_id, "kind": "binding_actor",
                         "binding_path": "/Game/Test/BP_Target", "generation": "1"}],
            "space": {"space_revision": self.space_revision,
                      "anchor": {"translation": [0.0, 0.0, 0.0],
                                 "rotation": [0.0, 0.0, 0.0, 1.0], "scale": 1.0}},
        })
        init = recv_message(sock)
        self.identity["init"] = init
        if init.get("type") != "init":
            self.errors.append("second message was {0!r}".format(init.get("type")))
            return
        if init.get("object_id") != self.object_id:
            self.errors.append("init object id {0!r}".format(init.get("object_id")))
        self._send(sock, {
            "type": "ready",
            "session_id": self.session_id,
            "object_id": self.object_id,
            "set_revision": init.get("set_revision", 1),
            "space_revision": init.get("space_revision", 1),
            "revision": init.get("revision"),
            "missing_in_unreal": [],
            "missing_in_maya": [],
            "bone_name_remaps": [],
            "workflow": init.get("workflow", "animation"),
            "target_morph_count": 1,
            "accepted_morph_count": 1,
        })
        driver = threading.Thread(target=self._drive, args=(sock,), daemon=True)
        driver.start()
        self._receive_loop(sock)
        self.stop.set()

    def _drive(self, sock):
        """Send targets; a new target never cancels the issued one."""
        frame = 1.0
        while not self.stop.is_set() and self.frame_serial < self.targets * 4:
            if self.mode != "linkage":
                time.sleep(0.005)
                continue
            if self.issued is None or self.frame_serial % self.pause_frames == 0:
                self.frame_serial += 1
                self.eval_serial += 1
                self.transport_serial += 1
                target = self._target(frame)
                try:
                    self._send(sock, target)
                except OSError as exc:
                    self.errors.append("send failed: {0}".format(exc))
                    return
                self.last_target_time = time.monotonic()
                if self.issued is None:
                    self.issued = target
                else:
                    self.pending = target
            frame += 1.0
            time.sleep(self.cadence)

    def _target(self, frame):
        playback_start = 1
        cut = "shotA" if frame < self.targets / 2 else "shotB"
        return {
            "type": "camera_time",
            "session_id": self.session_id,
            "set_revision": self.set_revision,
            "mode_revision": self.mode_revision,
            "space_revision": self.space_revision,
            "eval_id": "seq@{0}+0/cam#{1}".format(int(frame), cut),
            "eval_serial": self.eval_serial,
            "transport_serial": self.transport_serial,
            "frame_serial": self.frame_serial,
            "time": {
                "display_frame": float(frame),
                "source_frame": int(frame),
                "seconds": (frame - playback_start) / self.fps,
                "tick": int(frame * 1000),
                "display_rate": {"numerator": int(self.fps), "denominator": 1},
                "tick_resolution": {"numerator": int(self.fps * 1000),
                                    "denominator": 1},
                "playback_range": {"start": playback_start, "end": 240},
            },
            "camera": {
                "location": [0.0, 120.0, 240.0],
                "right": [1.0, 0.0, 0.0],
                "up": [0.0, 0.0, 1.0],
                "forward": [0.0, 1.0, 0.0],
                "focal_length_mm": 35.0,
                "sensor_width_mm": 36.0,
                "sensor_height_mm": 20.25,
                "sensor_horizontal_offset_mm": 0.0,
                "sensor_vertical_offset_mm": 0.0,
                "aspect_ratio": 16.0 / 9.0,
                "aspect_axis_constraint": "MaintainXFOV",
                "constrain_aspect_ratio": False,
                "aperture": 2.8,
                "focus_distance_cm": 240.0,
                "focus_method": "Disable",
                "squeeze_factor": 1.0,
                "near_clip_cm": 10.0,
                "near_clip_source": "engine",
            },
            "gate": {"output_width": 1920, "output_height": 1080,
                     "aperture_width": 1920, "aperture_height": 1080,
                     "pixel_aspect_ratio": 1.0},
            "cut": {"stage": "root", "name": cut},
        }

    def _receive_loop(self, sock):
        sock.settimeout(0.2)
        while not self.stop.is_set():
            try:
                message = recv_message(sock)
            except socket.timeout:
                self._check_convergence()
                continue
            except (EOFError, ValueError, OSError) as exc:
                if self.leaving is None:
                    self.errors.append("receive failed: {0}".format(exc))
                return
            self._handle(sock, message)

    def _handle(self, sock, message):
        message_type = message.get("type")
        if message_type == "mode":
            self.mode = message.get("mode", self.mode)
            self.identity.setdefault("modes", []).append(message)
            return
        if message_type == "leave_linkage":
            self.leaving = message
            self.mode = "realtime"
            self._send(sock, {
                "type": "mode_state",
                "session_id": self.session_id,
                "mode_revision": message.get("mode_revision"),
                "mode": "realtime",
                "control": "maya",
                "detail": "Maya restored its own state",
            })
            return
        if message_type == "reference_result":
            self.identity.setdefault("reference", []).append(message)
            return
        if message_type == "frame":
            # Realtime frames resume after linkage; the peer only counts them.
            self.identity["realtime_frames"] = self.identity.get("realtime_frames", 0) + 1
            return
        if message_type != "group":
            self.errors.append("unexpected message {0!r}".format(message_type))
            return
        for field in ("session_id", "set_revision", "space_revision", "mode_revision"):
            if message.get(field) != getattr(self, field):
                self.errors.append(
                    "group {0} {1!r} does not match {2!r}".format(
                        field, message.get(field), getattr(self, field)))
        self.groups.append(message)
        received = time.monotonic()
        if self.application_delay:
            time.sleep(self.application_delay)
        self.applied.append(message.get("eval_id"))
        if self.issued is not None and message.get("eval_id") == self.issued.get("eval_id"):
            self.latencies.append(received - self.last_target_time)
            self.issued = self.pending
            self.pending = None
        self._send(sock, {
            "type": "applied",
            "session_id": self.session_id,
            "mode_revision": self.mode_revision,
            "eval_id": message.get("eval_id"),
            "sample_serial": message.get("sample_serial"),
            "applied": True,
            "applied_display_frame": message.get("time", {}).get("ue_display_frame"),
            "lag_seconds": (self.pending is not None) * 0.05,
            "components": [{"object_id": self.object_id,
                            "bones": len(message.get("objects", [{}])[0].get(
                                "transforms", [])),
                            "morphs": len(message.get("objects", [{}])[0].get(
                                "curves", [])),
                            "parts": 0}],
            "reason": "",
        })
        self._check_convergence()

    def _check_convergence(self):
        if self.converged_at is not None:
            return
        if self.issued is None and self.pending is None and self.applied:
            self.converged_at = time.monotonic()

    def result(self):
        return {
            "ok": bool(self.applied) and not self.errors
                  and self.converged_at is not None
                  and self.leaving is not None,
            "session_id": self.session_id,
            "targets_sent": self.frame_serial,
            "groups": len(self.groups),
            "applied": len(self.applied),
            "errors": self.errors,
            "latency_seconds": {
                "p50": percentile(self.latencies, 0.50),
                "p95": percentile(self.latencies, 0.95),
                "p99": percentile(self.latencies, 0.99),
                "max": max(self.latencies) if self.latencies else None,
            },
            "convergence_seconds": (
                None if self.converged_at is None else self.converged_at - self.started),
            "leave": self.leaving,
            "identity": self.identity,
            "distinct_eval_ids": len(set(self.applied)),
        }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--targets", type=int, default=20)
    parser.add_argument("--delay-ms", type=float, default=0.0)
    parser.add_argument("--application-delay-ms", type=float, default=0.0)
    parser.add_argument("--cadence-ms", type=float, default=10.0)
    parser.add_argument("--fps", type=float, default=24.0)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--result", required=True)
    args = parser.parse_args(argv)

    peer = MockUnrealPeer(args.port, args.targets, args.delay_ms,
                          args.cadence_ms / 1000.0,
                          args.application_delay_ms / 1000.0, args.fps,
                          listen_timeout=args.timeout)
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", args.port))
    server.listen(1)
    server.settimeout(args.timeout)
    try:
        sock, _ = server.accept()
    except socket.timeout:
        peer.errors.append("no client connected before the timeout")
        with open(args.result, "w", encoding="utf-8") as handle:
            json.dump(peer.result(), handle)
        return 1
    with sock:
        try:
            peer.serve(sock)
        except Exception as exc:  # a broken client must still leave a report
            peer.errors.append("{0}: {1}".format(type(exc).__name__, exc))
    with open(args.result, "w", encoding="utf-8") as handle:
        json.dump(peer.result(), handle, ensure_ascii=False, indent=2)
    print(json.dumps(peer.result(), ensure_ascii=False, indent=2))
    return 0 if peer.result()["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
