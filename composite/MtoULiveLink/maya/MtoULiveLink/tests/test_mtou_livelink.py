import errno
import importlib.util
import math
import pathlib
import struct
import threading
import unittest
from unittest import mock

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "MtoULiveLink.py"
SPEC = importlib.util.spec_from_file_location("MtoULiveLink", str(SCRIPT))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class LatestFrameTests(unittest.TestCase):
    def test_only_newest_unsent_frame_is_kept(self):
        slot = MODULE._LatestFrame()
        slot.put(b"first")
        slot.put(b"second")
        self.assertEqual(b"second", slot.take())
        self.assertIsNone(slot.take())


class SenderLifecycleTests(unittest.TestCase):
    def test_runtime_structured_error_is_preserved(self):
        worker = MODULE._SenderWorker({"type": "init"})
        runtime_error = MODULE.encode_message({
            "type": "error",
            "code": "INVALID_MESSAGE",
            "message": "bad frame",
            "details": "transform count mismatch",
        })

        class RuntimeErrorSocket(object):
            def __init__(self):
                self.reply = MODULE.encode_message({
                    "type": "ready", "missing_in_unreal": [], "missing_in_maya": []
                }) + runtime_error

            def settimeout(self, timeout):
                pass

            def recv(self, size):
                chunk, self.reply = self.reply[:size], self.reply[size:]
                return chunk

            def sendall(self, packet):
                pass

            def close(self):
                pass

        fake_socket = RuntimeErrorSocket()
        with mock.patch.object(worker, "_connect", return_value=fake_socket), \
             mock.patch.object(worker, "_send_initial", return_value=True), \
             mock.patch.object(MODULE.select, "select", return_value=([fake_socket], [], [])):
            worker.run()

        state, _, _, diagnostic = worker.status()
        self.assertEqual("error", state)
        self.assertEqual("INVALID_MESSAGE", diagnostic["code"])
        self.assertEqual("transform count mismatch", diagnostic["details"])

    def test_stop_during_connection_prevents_init_and_terminates(self):
        entered = threading.Event()
        released = threading.Event()

        class BlockingSocket(object):
            def __init__(self):
                self.sent = []
                self._reply = MODULE.encode_message({"type": "ready"})

            def settimeout(self, timeout):
                pass

            def setblocking(self, blocking):
                pass

            def connect(self, address):
                entered.set()
                released.wait(2.0)

            def connect_ex(self, address):
                entered.set()
                released.wait(2.0)
                return errno.EINPROGRESS

            def shutdown(self, how):
                released.set()

            def close(self):
                released.set()

            def sendall(self, packet):
                self.sent.append(packet)

            def recv(self, size):
                chunk, self._reply = self._reply[:size], self._reply[size:]
                return chunk

        fake_socket = BlockingSocket()
        with mock.patch.object(MODULE.socket, "socket", return_value=fake_socket):
            worker = MODULE._SenderWorker({"type": "init"})
            worker.start()
            self.assertTrue(entered.wait(1.0))
            worker.stop()
            try:
                worker.join(0.2)
                self.assertFalse(worker.is_alive())
                self.assertEqual([], fake_socket.sent)
            finally:
                released.set()
                worker.join(1.0)

    def test_stop_after_connection_check_prevents_init(self):
        checked = threading.Event()
        released = threading.Event()

        class PausingEvent(object):
            def __init__(self):
                self._event = threading.Event()
                self._checks = 0

            def is_set(self):
                result = self._event.is_set()
                self._checks += 1
                if self._checks == 3:
                    checked.set()
                    released.wait(1.0)
                return result

            def set(self):
                self._event.set()

        class ReadySocket(object):
            def __init__(self):
                self.sent = []
                self._reply = MODULE.encode_message({"type": "ready"})

            def settimeout(self, timeout):
                pass

            def setblocking(self, blocking):
                pass

            def connect_ex(self, address):
                return 0

            def shutdown(self, how):
                pass

            def close(self):
                pass

            def sendall(self, packet):
                self.sent.append(packet)

            def recv(self, size):
                chunk, self._reply = self._reply[:size], self._reply[size:]
                return chunk

        fake_socket = ReadySocket()
        with mock.patch.object(MODULE.socket, "socket", return_value=fake_socket):
            worker = MODULE._SenderWorker({"type": "init"})
            worker._stop_event = PausingEvent()
            worker.start()
            self.assertTrue(checked.wait(1.0))
            worker.stop()
            released.set()
            worker.join(0.2)
            self.assertFalse(worker.is_alive())
            self.assertEqual([], fake_socket.sent)

    def test_stop_after_send_decision_prevents_init_write(self):
        decision_released = threading.Event()
        resume_write = threading.Event()

        class PausingLock(object):
            def __init__(self):
                self._lock = threading.Lock()
                self._releases = 0

            def __enter__(self):
                self._lock.acquire()
                return self

            def __exit__(self, exc_type, exc_value, traceback):
                self._releases += 1
                pause = self._releases == 2
                self._lock.release()
                if pause:
                    decision_released.set()
                    resume_write.wait(1.0)

        class PausingWriteSocket(object):
            def __init__(self):
                self.sent = []

            def settimeout(self, timeout):
                pass

            def setblocking(self, blocking):
                pass

            def connect_ex(self, address):
                return 0

            def shutdown(self, how):
                pass

            def close(self):
                pass

            def sendall(self, packet):
                self.sent.append(bytes(packet))

            def send(self, packet):
                if not resume_write.is_set():
                    raise OSError(errno.EWOULDBLOCK, "send would block")
                self.sent.append(bytes(packet))
                return len(packet)

        fake_socket = PausingWriteSocket()

        def pause_writable(readable, writable, errors, timeout):
            resume_write.wait(timeout)
            return [], writable, []

        with mock.patch.object(MODULE.socket, "socket", return_value=fake_socket), \
             mock.patch.object(MODULE.select, "select", side_effect=pause_writable):
            worker = MODULE._SenderWorker({"type": "init"})
            worker._lock = PausingLock()
            worker.start()
            self.assertTrue(decision_released.wait(1.0))
            worker.stop()
            resume_write.set()
            worker.join(0.2)
            self.assertFalse(worker.is_alive())
            self.assertEqual([], fake_socket.sent)

    def test_stop_closes_socket_while_initial_send_blocks(self):
        send_started = threading.Event()
        release_send = threading.Event()
        stop_finished = threading.Event()

        class BlockingSendSocket(object):
            def __init__(self):
                self._reply = MODULE.encode_message({"type": "ready"})
                self.closed = False

            def settimeout(self, timeout):
                pass

            def setblocking(self, blocking):
                pass

            def connect_ex(self, address):
                return 0

            def shutdown(self, how):
                self.closed = True
                release_send.set()

            def close(self):
                self.closed = True
                release_send.set()

            def sendall(self, packet):
                send_started.set()
                release_send.wait(2.0)

            def send(self, packet):
                send_started.set()
                raise OSError(errno.EWOULDBLOCK, "send would block")

            def recv(self, size):
                chunk, self._reply = self._reply[:size], self._reply[size:]
                return chunk

        fake_socket = BlockingSendSocket()

        def block_writable(readable, writable, errors, timeout):
            release_send.wait(2.0)
            return [], writable, []

        with mock.patch.object(MODULE.socket, "socket", return_value=fake_socket), \
             mock.patch.object(MODULE.select, "select", side_effect=block_writable):
            worker = MODULE._SenderWorker({"type": "init"})
            worker.start()
            self.assertTrue(send_started.wait(1.0))
            stopper = threading.Thread(target=lambda: (worker.stop(), stop_finished.set()))
            stopper.start()
            try:
                self.assertTrue(stop_finished.wait(0.2))
                self.assertTrue(fake_socket.closed)
                worker.join(0.2)
                self.assertFalse(worker.is_alive())
            finally:
                release_send.set()
                stopper.join(1.0)
                worker.join(1.0)

    def test_initial_send_times_out_during_continuous_partial_writes(self):
        class FakeClock(object):
            now = 0.0

            def time(self):
                return self.now

        clock = FakeClock()

        class PartialSendSocket(object):
            def send(self, packet):
                clock.now += 1.0
                return 1

        worker = MODULE._SenderWorker({"type": "init"})
        with mock.patch.object(MODULE.time, "time", side_effect=clock.time):
            with self.assertRaisesRegex(MODULE.socket.timeout,
                                        "timed out sending init message"):
                worker._send_initial(PartialSendSocket())

        self.assertEqual(5.0, clock.now)


class ControllerLifecycleTests(unittest.TestCase):
    def test_main_window_always_fits_its_controls(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.currentUnit.return_value = "film"
        fake_cmds.control.return_value = True

        controller = MODULE._Controller()
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller.build_ui()

        create_call = fake_cmds.window.call_args_list[0]
        self.assertEqual(MODULE.WINDOW_NAME, create_call.args[0])
        self.assertEqual(430, create_call.kwargs["width"])
        self.assertTrue(create_call.kwargs["resizeToFitChildren"])
        self.assertNotIn("height", create_call.kwargs)
        self.assertNotIn("widthHeight", create_call.kwargs)

    def test_connect_rolls_back_worker_and_registered_callbacks_on_setup_error(self):
        class FakeWorker(object):
            instances = []

            def __init__(self, init_message):
                self.started = False
                self.stopped = False
                self.joined = None
                self.__class__.instances.append(self)

            def start(self):
                self.started = True

            def stop(self):
                self.stopped = True

            def join(self, timeout):
                self.joined = timeout

        class FailingSceneMessage(object):
            kBeforeNew = 1
            kBeforeOpen = 2
            kMayaExiting = 3
            calls = 0

            @classmethod
            def addCallback(cls, message, callback):
                cls.calls += 1
                if cls.calls == 2:
                    raise RuntimeError("scene callback registration failed")
                return 101

        removed = []

        class FakeMessage(object):
            @staticmethod
            def removeCallback(callback_id):
                removed.append(callback_id)

        class FakeDagPath(object):
            @staticmethod
            def node():
                return object()

        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": FailingSceneMessage,
            "MNodeMessage": type("FakeNodeMessage", (), {}),
            "MTimerMessage": type("FakeTimerMessage", (), {}),
            "MMessage": FakeMessage,
        })
        subject = {"root": "|root", "bones": [{"name": "root", "parent": -1,
                                                      "dag_path": FakeDagPath()}],
                   "curves": []}
        controller = MODULE._Controller()
        controller._role = {"root": "|root", "display": {"plug": "Display_ctrl.clothes"}}
        fake_cmds = type("FakeCmds", (), {
            "currentUnit": staticmethod(lambda **kwargs: "film"),
            "confirmDialog": staticmethod(lambda **kwargs: "确定"),
        })
        with mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "_capture_subject", return_value=subject), \
             mock.patch.object(MODULE, "_SenderWorker", FakeWorker):
            controller.connect()

        worker = FakeWorker.instances[0]
        self.assertTrue(worker.started)
        self.assertTrue(worker.stopped)
        self.assertEqual(1.0, worker.joined)
        self.assertEqual([101], removed)
        self.assertIsNone(controller._worker)
        self.assertIsNone(controller._subject)
        self.assertIsNone(controller._timer_id)
        self.assertEqual([], controller._connection_callback_ids)


class ProtocolTests(unittest.TestCase):
    def test_protocol_v2_init_and_structured_diagnostics(self):
        message = MODULE.make_init_message([["root", -1]], ["Smile"])
        self.assertEqual(2, message["version"])
        diagnostic = MODULE.make_diagnostic(
            "SKELETON_MISMATCH", "Skeleton differs", details="details"
        )
        self.assertEqual("SKELETON_MISMATCH", diagnostic["code"])
        self.assertEqual("骨架与 Unreal Skeletal Mesh 不匹配", diagnostic["summary"])
        self.assertIn("UE", diagnostic["solution"])

    def test_maya_time_units_keep_exact_animation_frame_rates(self):
        expected = {
            "game": 15.0,
            "film": 24.0,
            "pal": 25.0,
            "ntsc": 30.0,
            "show": 48.0,
            "palf": 50.0,
            "ntscf": 60.0,
            "23.976fps": 23.976,
            "29.97fps": 29.97,
            "59.94fps": 59.94,
            "29.97df": 29.97,
        }
        for unit, fps in expected.items():
            with self.subTest(unit=unit):
                self.assertAlmostEqual(fps, MODULE.frames_per_second(unit))
        self.assertEqual("23.976 fps", MODULE.format_fps(23.976))
        self.assertEqual("24 fps", MODULE.format_fps(24.0))
        with self.assertRaisesRegex(ValueError, "1.*60"):
            MODULE.validate_frame_rate(120.0)

    def test_ready_reply_keeps_both_blendshape_difference_lists(self):
        reply = {
            "type": "ready",
            "missing_in_unreal": ["MayaOnly"],
            "missing_in_maya": ["UnrealOnly"],
        }
        warning = MODULE.blendshape_warning_from_reply(reply)
        self.assertEqual(["MayaOnly"], warning["missing_in_unreal"])
        self.assertEqual(["UnrealOnly"], warning["missing_in_maya"])
        self.assertTrue(warning["has_warning"])

    def test_ready_reply_reports_unreal_bone_name_remapping(self):
        warning = MODULE.blendshape_warning_from_reply({
            "type": "ready",
            "missing_in_unreal": [],
            "missing_in_maya": [],
            "bone_name_remaps": ["hair_7/tip_2 -> tip_21"],
        })
        self.assertEqual(
            ["hair_7/tip_2 -> tip_21"], warning["bone_name_remaps"])
        self.assertTrue(warning["has_warning"])

    def test_namespace_normalization(self):
        self.assertEqual("spine_01", MODULE.normalize_name("|Rig|Hero:spine_01"))
        self.assertEqual("jaw", MODULE.normalize_name("|Rig|show:Hero:jaw"))

    def test_hierarchy_is_parent_first_and_rejects_duplicates(self):
        children = {"|root": ["|root|a", "|root|b"], "|root|a": ["|root|a|tip"]}
        records = MODULE.build_hierarchy("|root", lambda path: children.get(path, []))
        self.assertEqual(["root", "a", "tip", "b"], [record["name"] for record in records])
        self.assertEqual([-1, 0, 1, 0], [record["parent"] for record in records])
        with self.assertRaisesRegex(
                MODULE.DuplicateBoneNamesError,
                "duplicate normalized bone names: arm") as caught:
            MODULE.build_hierarchy(
                "|root",
                lambda path: ["|root|A:arm", "|root|B:arm"]
                if path == "|root" else [],
            )
        self.assertEqual(["|root|A:arm", "|root|B:arm"], caught.exception.paths)
        diagnostic = MODULE.duplicate_bone_diagnostic(caught.exception)
        self.assertEqual("DUPLICATE_BONE_NAMES", diagnostic["code"])
        self.assertEqual(caught.exception.paths, diagnostic["duplicate_paths"])
        self.assertIn("|root|A:arm", diagnostic["details"])
        allowed = MODULE.build_hierarchy(
            "|root",
            lambda path: ["|root|A:arm", "|root|B:arm"]
            if path == "|root" else [],
            allow_duplicates=True,
        )
        self.assertEqual(["root", "arm", "arm"], [item["name"] for item in allowed])

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
