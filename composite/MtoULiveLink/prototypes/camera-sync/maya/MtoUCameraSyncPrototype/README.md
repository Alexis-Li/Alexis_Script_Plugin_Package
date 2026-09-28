# MtoU camera sync prototype: Maya side

Follower for the bounded Unreal -> Maya camera sync prototype described in
`../../protocol.md`. Unreal owns the camera, the time and the output
resolution; this side applies each received frame to one disposable Maya
camera, moves Maya's current time to the reported position, and answers with
the values it actually holds plus Maya's own marker NDC for comparison.
`protocol.md` is the frozen contract; this folder implements the Maya half of
it and never changes it.

Nothing here imports or depends on the product's `MtoULiveLink.py`.

## Files

| Path | Contents |
| --- | --- |
| `scripts/mtou_camera_sync_mapping.py` | Pure mapping: axis conversion, film back, film fit, time model, Maya's film-aperture projection, payload validation. No Maya imports, standard library only. |
| `scripts/MtoUCameraSyncPrototype.py` | Maya follower, socket client, idle/`pump()` timeline driver and the `run()` entry point. |
| `tests/test_camera_sync_mapping.py` | Pure unit tests; runs under CPython 3 and under mayapy. |
| `tests/maya_host_camera_tests.py` | mayapy host checks in a disposable scene plus the opt-in Arnold render check. |
| `tests/maya_camera_sync_peer.py` | Opt-in cross-host peer: connects to a real publisher and writes a phase tracked result JSON. |
| `tests/mock_camera_sync_server.py` | Fixture driven fake Unreal publisher, and the fixture authoring side (including the Unreal-side marker NDC). |
| `tests/maya_render_marker_check.py` | Rendered framing check: renders the synced camera with Arnold and compares marker centroids with the projection maths. |
| `tests/__init__.py` | Makes the folder importable for unittest discovery. |

## Running

`mayapy` is Maya's standalone Python interpreter. It is not on `PATH` by
default; point the commands at your Maya installation, for example via a
`MAYAPY` environment variable (`"$MAYAPY"` below).

```bash
# 1. pure mapping tests (any CPython 3)
python -m unittest discover \
  -s composite/MtoULiveLink/prototypes/camera-sync/maya/MtoUCameraSyncPrototype/tests \
  -t composite/MtoULiveLink/prototypes/camera-sync/maya/MtoUCameraSyncPrototype

# 2. mayapy host checks (disposable scene, JSON evidence)
"$MAYAPY" composite/MtoULiveLink/prototypes/camera-sync/maya/MtoUCameraSyncPrototype/tests/maya_host_camera_tests.py \
  --result host_result.json
#    ... add --render to also run the Arnold framing check
"$MAYAPY" .../tests/maya_host_camera_tests.py --result host_result.json --render --render-dir render_evidence

# 3. cross-host peer against a real publisher
"$MAYAPY" .../tests/maya_camera_sync_peer.py --fixture fixture.json --result result.json

# 4. the same peer against the bundled mock publisher (no Unreal needed)
"$MAYAPY" .../tests/maya_camera_sync_peer.py --fixture fixture.json --result result.json --spawn-mock
#    or with the mock as a separate process:
python .../tests/mock_camera_sync_server.py --write-fixture fixture.json --frames 3 --port 54388
python .../tests/mock_camera_sync_server.py --fixture fixture.json --applied-log applied.json
"$MAYAPY" .../tests/maya_camera_sync_peer.py --fixture fixture.json --result result.json
```

`MtoUCameraSyncPrototype.py` can also be run on its own, which is the manual
path for a live viewport session:

```bash
"$MAYAPY" .../scripts/MtoUCameraSyncPrototype.py --host 127.0.0.1 --port 54330 \
  --duration 30 --camera-name MtoU_UE_Camera --result session.json
#    --idle-pump drives the session from Maya's idle event (interactive Maya)
```

## What the Maya side accepts and sends

The wire keys are fixed by agreement with the Unreal side; unknown extra keys
in the payload are ignored, never rejected. A message with an unknown `type` or
a missing required field is answered with a stable error category
(`UNKNOWN_MESSAGE_TYPE`, `MISSING_FIELD`, `INVALID_FIELD`,
`UNSUPPORTED_PROTOCOL`, `UNSUPPORTED_TIME_AUTHORITY`) and **no** part of that
frame is applied.

* `session`: `protocol` (`MtoUCameraSync`), `version` (1), `port`,
  `time_authority` (`unreal`), `sequence`, `display_rate{numerator,
  denominator}`, `tick_resolution`, `playback_range{start,end}`,
  `output_resolution{x,y}`, `camera_cut`, and optionally
  `far_clip_fallback_cm` and `marker_names`.
* `frame`: `session`, `sequence`, `frame_serial`, `time{display_frame,
  seconds, source_frame, tick, display_rate}`, `camera_cut`,
  `output_resolution`, `camera`, `view`, `projection`, `markers[...]`. Every
  frame is a complete state, so a dropped frame never leaves a partial camera.
* `camera`: `location{x,y,z}` plus either `right`/`up`/`forward` or
  `rotation{roll,pitch,yaw}`; required `focal_length_mm`, `sensor_width_mm`,
  `sensor_height_mm`; optional `sensor_horizontal_offset_mm`,
  `sensor_vertical_offset_mm`, `aspect_axis_constraint`, `constrain_aspect_ratio`,
  `f_stop`, `focus_distance_cm`, `depth_of_field`, `squeeze_factor`,
  `near_clip_cm`, `far_clip_cm`.
* `markers`: `name`, `location{x,y,z}`, `ndc{x,y}` (`-1..1`, `+Y` up).
* Client messages: `hello` (with `scene_fps` from
  `cmds.currentUnit(query=True, time=True)` and the unit name), `applied`,
  `bye`. Anything else the client sends is refused by the publisher, which is
  the protocol's structural guarantee that Maya cannot steer Unreal's time; the
  follower records such an `error` line and keeps following.

`applied` carries `session`, `sequence`, `frame_serial`, `maya_frame`,
`status` (`applied`, `applied_quantized` or `rejected`), `detail`,
`markers[{name, unreal_ndc, maya_ndc, delta}]` and `camera`, whose keys are the
Maya attribute names read back from the node (`focalLength`,
`horizontalFilmAperture`, `verticalFilmAperture`, `horizontalFilmOffset`,
`verticalFilmOffset`, `filmFit`, `lensSqueezeRatio`, `fStop`, `focusDistance`,
`depthOfField`, `nearClipPlane`, `farClipPlane`, `displayResolution`,
`displayGateMask`, `defaultResolution`), plus `transform`, `shape`,
`maya_time`, `applied_time`, `quantized`, `far_clip_substituted`,
`far_clip_source`, `film_fit`, `lens_squeeze_ratio` and `world_matrix`.

The peer's result JSON renames those to the Unreal side's vocabulary
(`focal_length_mm`, `horizontal_film_aperture_in`, `film_fit`,
`horizontal_film_offset_in`, `f_stop`, `focus_distance_cm`, `depth_of_field`,
`near_clip_cm`, `far_clip_used_cm`, `lens_squeeze_ratio`, `world_matrix`,
`maya_time`) so the Unreal test can assert directly.

## Mapping rules worth knowing

* Axis and units: `ue (x, y, z) -> maya (y, z, -x)`; the world matrix rows are
  `X = right`, `Y = up`, `Z = -forward` and the translation, all in
  centimetres. A scene that is not in centimetres, or whose time unit is not a
  frame rate, is refused with a clear status instead of being rescaled or
  guessed.
* Sensor millimetres are converted to Maya film-aperture and film-offset inches
  (`/ 25.4`), signs preserved. Unreal's positive sensor offset and Maya's
  positive film offset are treated as the same physical displacement, as
  `protocol.md` specifies; the real cross-host session is what confirms the two
  hosts agree on that sign.
* Film fit: `MaintainXFOV -> Horizontal`, `MaintainYFOV -> Vertical`,
  `MaintainMajorAxisFOV -> Horizontal/Vertical` by whichever axis is larger in
  the aspect Unreal actually renders, and no declared constraint -> `Fill`. The
  `Fill` branch was corrected against rendered images: Maya inscribes the
  resolution gate in the film gate, so with a gate wider than the sensor the
  horizontal aperture is exact, otherwise the vertical one is.
* Time: `maya_time = maya_start_frame + (display_frame - playback_start) *
  scene_fps / display_rate`. A non-integral result is reported as
  `quantized: true` and applied at the nearest integer frame; Maya never
  silently keeps a stale frame.
* Far clip: Unreal has none. Maya takes the payload's `far_clip_cm`, else the
  session's `far_clip_fallback_cm`, else a documented prototype default
  (`100000.0` cm), and reports the substitution in `detail` and in
  `far_clip_substituted` / `far_clip_source`.
* Resolution gate: the payload's pixel extent is written to
  `defaultResolution.width/height`, and `deviceAspectRatio` is written too.
  Maya's film fit reads the device aspect attribute, not the pixel extent, so
  leaving it alone would keep framing with the previous aspect ratio whenever a
  non-16:9 gate is applied.
* Marker NDC uses Maya's own film-aperture projection (see the module
  docstring for the formula); the delta reported is `maya_ndc - unreal_ndc`.

## Scene safety

* The scene is never saved, no keys are created, the playback range and the
  frame rate are never changed, and no scriptJob or callback survives a
  session.
* Every frame's camera writes and its time move share one undo chunk; a failure
  inside the chunk is rolled back, so a rejected frame leaves the previous
  state exactly as it was. A frame that repeats the previous state (the
  publisher's heartbeat) does not touch Maya at all, which keeps the undo queue
  clean.
* `stop()` restores the Maya current time captured at session start, deletes
  the camera this session created (a pre-existing node of the same name is
  reused, never deleted), detaches the idle pump and reports a summary. A lost
  connection reports the reason and leaves Maya usable.

## Environment notes from this machine

Verified with Maya 2024 (mayapy 3.10.8) and Arnold/mtoa 5.3.4.1.

* `cmds.scriptJob(idleEvent=...)` returns `None` in batch mayapy: batch Maya
  has no UI and delivers no idle events. The follower therefore attaches an
  idle pump only when Maya reports a UI, reports the reason when it cannot, and
  `pump()` is the headless path. `tests/maya_host_camera_tests.py` verifies the
  callback body itself and that no scriptJob survives `stop()`.
* `MImage.pixels()` and `MImage.floatPixels()` return raw pointers (an `int`)
  in this Maya build rather than a Python sequence, so the render check decodes
  Maya's Targa output with the standard library instead: no third party decoder
  is installed (no numpy, no PIL), and the format is selected from
  `defaultRenderGlobals.imageFormat`, whose enum entries can carry explicit
  values (`PNG=32`), so the names are parsed rather than indexed.

## Evidence

`tests/maya_host_camera_tests.py` (56 checks) asserts, on a disposable scene
with a non-zero playback range: the axes and units, every mapped camera
attribute and its units, the film fit, the resolution gate, the world matrix
read back with `cmds.xform(query=True, worldSpace=True, matrix=True)`, the
applied time (including quantization), that the marker NDC agrees with the
Unreal NDC the payload carried, that no keys appear and the playback range is
unchanged, that a frame missing a field is rejected without touching the
camera, that `stop()` restores the time and removes the camera, a full socket
session against the mock publisher (including a refused second connection and a
refused client-controlled time message), that a lost connection reports its
reason while leaving Maya usable, and -- with `--render` -- the Arnold framing
check.

The render check renders the synced camera at the declared output resolution
with emissive marker spheres on black, locates each marker's centroid in the
image, and compares it with Maya's own projection and with the Unreal NDC. On
this machine it passes for the payload's `Horizontal` fit (0.15 px) and for
`Fill` and `Vertical` variant configurations (0.09 px, 0.13 px, 0.20 px), well
inside the 2 px tolerance; markers the variant framings push out of the image
are reported as skipped rather than matched.

## Real cross-host outcome

The Unreal publisher drove this follower over a real socket with two camera
cuts, a non-zero playback start, and six published frames. All six applied with
status `applied`, none was rejected or quantized, the read-back focal length,
film aperture, film fit, f-stop, focus distance, and gate matched the payload,
Maya's marker NDC differed from Unreal's by at most `1.09e-07`, `stop()` restored
the previous current time, removed the created camera, and left the playback
range and every existing key unchanged.

Two client lines carried extra bytes after a complete message in that session
(28112 bytes discarded, recorded by the publisher). The follower's own output is
byte-clean against a plain Python server, so the anomaly is attributed to the
Maya process on this host rather than to the wire format, and it never changed
an applied value.

Also measured: the resolution gate must follow the payload's
`aperture_resolution` rather than its full `output_resolution`. Maya derives the
axis it does not keep from the gate aspect, so a 4:3 aperture in a 16:9 gate
framed 0.25–0.38 NDC away from Unreal until the gate was set to the aperture
extent.
