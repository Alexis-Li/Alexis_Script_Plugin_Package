import copy
import json
import socket
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
PROTO = ROOT / 'composite/MtoULiveLink/prototypes/camera-sync/maya/MtoUCameraSyncPrototype'
sys.path[:0] = [str(PROTO / 'scripts'), str(PROTO / 'tests')]
import maya.standalone
maya.standalone.initialize(name='python')
import maya.cmds as cmds
import MtoUCameraSyncPrototype as sync
import mock_camera_sync_server as mock

OUTPUT = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT.parent / '.tmp' / 'issue52-review-repro.json'
OUTPUT.parent.mkdir(parents=True, exist_ok=True)
results = {'maya_version': cmds.about(version=True), 'cases': {}}

def fresh():
    cmds.file(new=True, force=True)
    cmds.currentUnit(linear='cm', time='film')
    cmds.currentTime(17)
    cmds.undoInfo(state=True)
    cmds.setAttr('defaultResolution.width', 800)
    cmds.setAttr('defaultResolution.height', 600)
    cmds.setAttr('defaultResolution.pixelAspect', 1)
    cmds.setAttr('defaultResolution.deviceAspectRatio', 4.0/3.0)

def snapshot():
    return {'time': cmds.currentTime(query=True),
            'resolution': {a: cmds.getAttr('defaultResolution.' + a)
                           for a in ('width', 'height', 'pixelAspect', 'deviceAspectRatio')},
            'cameras': cmds.ls(type='camera')}

fixture = mock.build_fixture(frames_to_apply=1, maya_start_frame=1001.0)
frame = copy.deepcopy(fixture['frames'][0])
frame.update(type='frame', session=fixture['session']['sequence'], sequence=1)
session = mock.session_message(fixture, 0)

def direct(name):
    follower = sync.CameraSyncFollower(camera_name=name)
    follower._scene_fps = 24
    follower._start_time = float(cmds.currentTime(query=True))
    follower._undo_state = bool(cmds.undoInfo(query=True, state=True))
    follower.session = copy.deepcopy(session)
    return follower

# Normal public entry point loses the publisher only after a real frame was applied.
fresh()
before = snapshot()
server = socket.socket()
server.bind(('127.0.0.1', 0))
server.listen(1)
port = server.getsockname()[1]
server_record = {}
def publisher():
    conn, _ = server.accept()
    conn.settimeout(5)
    stream = conn.makefile('rb')
    server_record['hello'] = json.loads(stream.readline())
    conn.sendall((json.dumps(session) + '\n').encode())
    time.sleep(0.15)
    conn.sendall((json.dumps(frame) + '\n').encode())
    server_record['applied'] = json.loads(stream.readline())
    time.sleep(0.1)
    stream.close()
    conn.shutdown(socket.SHUT_RDWR)
    conn.close()
    server.close()
t = threading.Thread(target=publisher)
t.start()
follower = sync.run(port=port, duration=5, camera_name='ReviewDisconnect')
t.join(5)
results['cases']['public_run_disconnect'] = {
    'before': before, 'after_return': snapshot(), 'state': follower.state,
    'socket_still_owned': follower.connected,
    'server_saw_applied': server_record.get('applied', {}).get('status'),
    'cleanup_after_observation': follower.stop()}

# Borrowed static camera and a locked scene resolution plug.
fresh()
transform, shape = cmds.camera(name='ReviewBorrowed')
cmds.setAttr(shape + '.focalLength', 29)
plugs = [shape + '.focalLength', 'defaultResolution.width']
for plug in plugs:
    cmds.setAttr(plug, lock=True)
before_locks = {p: cmds.getAttr(p, lock=True) for p in plugs}
follower = direct(transform)
applied = follower.apply_frame(frame)
stopped = follower.stop()
results['cases']['borrowed_locks'] = {
    'status': applied['status'], 'before': before_locks,
    'after': {p: cmds.getAttr(p, lock=True) for p in plugs},
    'restored_focal': cmds.getAttr(shape + '.focalLength'), 'stop': stopped}

# Borrowing an existing animated camera must not rewrite its animation.
fresh()
transform, shape = cmds.camera(name='ReviewAnimated')
cmds.setKeyframe(transform, attribute='translateX', time=1, value=10)
cmds.setKeyframe(transform, attribute='translateX', time=30, value=90)
cmds.setKeyframe(shape, attribute='focalLength', time=1, value=28)
cmds.setKeyframe(shape, attribute='focalLength', time=30, value=70)
cmds.currentTime(17)
before_keys = {p: cmds.keyframe(p, query=True, valueChange=True)
               for p in (transform + '.translateX', shape + '.focalLength')}
follower = direct(transform)
applied = follower.apply_frame(frame)
read_back = applied.get('camera', {})
_, actual_notes = sync.mapping().camera_parameters(frame['camera'], session,
                                                  (1920, 1080), frame)
actual_notes['focal_length_mm'] = read_back['focalLength']
actual_notes['aperture_inches'] = (read_back['horizontalFilmAperture'],
                                  read_back['verticalFilmAperture'])
actual_notes['film_offset_inches'] = (read_back['horizontalFilmOffset'],
                                     read_back['verticalFilmOffset'])
actual_notes['film_fit'] = read_back['filmFit']
actual_notes['lens_squeeze_ratio'] = read_back['lensSqueezeRatio']
actual_markers = sync.mapping().marker_report(frame['markers'], read_back['world_matrix'],
                                            actual_notes, (1920, 1080))
stated_delta = max(abs(v) for m in applied['markers'] for v in m['delta'])
read_back_delta = max(abs(v) for m in actual_markers for v in m['delta'])
stopped = follower.stop()
after_keys = {p: cmds.keyframe(p, query=True, valueChange=True) for p in before_keys}
results['cases']['borrowed_animation'] = {
    'status': applied['status'], 'detail': applied['detail'],
    'reported_camera': applied.get('camera'), 'before_keys': before_keys,
    'after_keys': after_keys, 'stop': stopped,
    'expected_focal': frame['camera']['focal_length_mm'],
    'expected_translate_x': frame['camera']['location']['y'],
    'reported_marker_max_delta': stated_delta,
    'read_back_marker_max_delta': read_back_delta}

# A refused connection also must restore the undo policy it enabled itself.
fresh()
probe = socket.socket()
probe.bind(('127.0.0.1', 0))
unused_port = probe.getsockname()[1]
cmds.undoInfo(state=False)
try:
    sync.run(port=unused_port, duration=0.1)
except sync.SyncRefused as error:
    results['cases']['connection_refused_undo'] = {
        'before_undo': False, 'after_undo': bool(cmds.undoInfo(query=True, state=True)),
        'error': str(error)}
probe.close()
OUTPUT.write_text(json.dumps(results, indent=2), encoding='utf-8')
print(json.dumps(results, indent=2))
maya.standalone.uninitialize()
