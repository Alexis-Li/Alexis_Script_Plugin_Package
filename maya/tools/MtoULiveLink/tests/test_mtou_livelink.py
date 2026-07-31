import importlib.util
import errno
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


class ControllerLifecycleTests(unittest.TestCase):
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
        with mock.patch.object(MODULE, "cmds", object()), \
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
        self.assertEqual([], controller._callback_ids)


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
