# Camera sync prototype: wire contract and parameter mapping

Scope: this document fixes the prototype's wire schema and its Unreal → Maya
parameter mapping so both hosts can be implemented and checked independently.
It is not a product protocol; the MtoU Live Link protocol stays at v9 and is not
touched by the prototype. The prototype's own version is **2**: version 1 had no
evaluation identity, so its receipt pairing depended on the transport serial.

## Roles

- **Unreal** is the camera and time authority. It owns the evaluated camera,
  the Level Sequence time, the output resolution, and the projection used to
  place markers on screen.
- **Maya** is the follower. It applies the received camera to one disposable
  scene camera and moves its current time to the reported position.
- Maya never sends camera or time commands back. Its application report can
  include a keyed joint pose witness sampled at the applied time; Unreal pairs
  that witness with the published camera frame before moving a disposable test actor.

## Channel

- TCP over loopback. Unreal binds and listens on port **54330**; Maya connects.
  The product's Live Link channel uses port 54321, so the prototype cannot
  collide with a running MtoU session.
- One JSON object per line, UTF-8, terminated by `\n`. No length prefix.
- One client at a time. A second connection is refused with `SESSION_BUSY`.
- Both sides ignore unknown fields, so a newer host never breaks an older one.

### Server → client

| `type` | When | Payload |
| --- | --- | --- |
| `session` | once after `hello` | `protocol`, `version`, `session`, `port`, `time_authority`, `sequence`, `display_rate`, `tick_resolution`, `playback_range`, `output_resolution`, `aperture_resolution`, `resolution_source`, `far_clip_fallback_cm`, `camera_cut`, `marker_names` |
| `frame` | every update | `session`, `frame_serial`, `eval_serial`, `eval_identity`, `sequence`, `time`, `camera_cut`, `output_resolution`, `aperture_resolution`, `resolution_source`, `camera`, `view`, `projection`, `markers` |
| `error` | on a rejected message | `category`, `detail` |
| `end` | on shutdown | `reason` |

### Client → server

| `type` | Purpose |
| --- | --- |
| `hello` | `protocol`, `version`, `host`, `scene_fps`, `time_unit` |
| `applied` | `session`, `sequence`, `frame_serial`, `eval_serial`, `eval_identity`, `maya_frame`, `maya_origin_frame`, `unreal_display_frame`, `unreal_camera_path`, `status`, `detail`, `camera`, `gate`, `markers`; the opt-in test peer also sends `pose{sampled_maya_frame,translate_x}` |
| `bye` | leave the session |

Any other client message is answered with
`{"type":"error","category":"CLIENT_MAY_NOT_CONTROL_TIME"}` and changes no
Unreal state. This is the prototype's structural guarantee that there is no
feedback loop: the `time` field is server-owned, and the test asserts that the
rejection list stays empty for a well-behaved client.

The contract is one JSON object per line. A malformed line with a complete
initial object is recorded with its discarded UTF-8 byte count
(`client_line_anomalies`, `client_trailing_bytes`,
`last_client_line_anomaly`). The initial object may still be used as a camera
application report, but its pose is never paired. A new TCP connection gets a
new `session` identity, so a report from a former connection cannot be paired
even when every other identity field is current.

### Evaluation identity and transport serial

`frame_serial` counts publications and always increases, including heartbeats
that repeat an unchanged target. `eval_serial` counts **evaluation targets**:
it increases only when the sampled target actually changes (a jump, a camera
cut, a shot change, a different sequence time or a different evaluated camera).
`eval_identity` is the readable form of that target:
`<sequence>@<tick>/<camera object path>`.

A keyed-joint witness is paired when its report answers the target sampled for
the current pump:

- Unreal samples the target, then processes client reports, then publishes. A
  report is therefore judged against the target that is current *now*, not
  against whichever publication it happened to answer. A report the timeline
  has already left — for example one that arrives in the same tick that moved
  from shot A to shot B, before B's frame was published — is refused and
  counted.
- `session`, `eval_serial`, `eval_identity`, `sequence`, `unreal_display_frame`,
  `unreal_camera_path`, `maya_frame` and `pose.sampled_maya_frame` must all
  match, and `frame_serial` must be a publication of this connection that was
  not paired before. A report may answer an earlier heartbeat of the same
  target: heartbeats repeat a target and do not supersede work in flight.
- Replaying the same publication never pairs twice, and a new connection never
  inherits the previous connection's pairing history.
- Acceptance is convergence, not one lucky pair: after the timeline stops, the
  target that is current must be answered within a bounded wait. `eval_serial`
  and `last_paired_eval_serial` expose that condition, and the tests assert it.
- Transport work is bounded per pump (64 client lines; the remainder stays in
  the receive buffer for the next tick) and the report evidence queue is capped
  (512 entries, evictions counted as `applied_reports_dropped`), so a slow or
  flooding client cannot grow an unbounded backlog of stale targets.

## Time model

- `display_rate` and `tick_resolution` are integer rationals (`numerator`,
  `denominator`).
- `playback_range` is the sequence's playback range in display frames; it may
  start at a non-zero frame.
- `time` carries `display_frame` (float, authoritative), `source_frame`
  (integer display frame), `seconds` (seconds since `playback_range.start`),
  `tick` (evaluation tick), plus the two rates again for convenience.
- Maya's applied time is

  `maya_time = maya_origin_frame + (display_frame - playback_range.start) *
  scene_fps / display_rate`

  where `maya_origin_frame` is an explicit follower setting, defaulting to the
  published range start. It never depends on Maya's current frame at connect.
  Equal rates therefore keep frame numbers aligned by default. Differing rates
  preserve elapsed time from the two origins. Maya 2024 evaluates fractional
  frames directly; `subframe` reports them without rounding.
- Exiting the session restores the Maya current time captured at start and
  releases the timeline. Neither host writes animation assets, keys, or
  playback ranges.

## Axis and unit conversion

Unreal is left-handed, centimetres, `+X` forward, `+Y` right, `+Z` up. Maya is
right-handed, centimetres, `+X` right, `+Y` up, `-Z` forward.

```
ue (x, y, z)  ->  maya (y, z, -x)
```

The camera's world transform is transferred by mapping its three basis vectors
and its translation, then writing the resulting Maya world matrix with columns
`X = right`, `Y = up`, `Z = -forward`. Both hosts use centimetres, so no scale
factor applies; a Maya scene whose linear unit is not centimetres is reported
as unsupported rather than silently rescaled. Maya reads its time unit
(`scene_fps`) from the scene and reports it in the handshake.

## Camera parameter mapping

| Unreal | Maya | Class |
| --- | --- | --- |
| evaluated view location, basis vectors | camera transform world matrix | direct |
| `CurrentFocalLength` (mm) | `focalLength` (mm) | direct |
| `Filmback.SensorWidth` / `SensorHeight` (mm) | `horizontalFilmAperture` / `verticalFilmAperture` (inches, `/ 25.4`) | adapted (unit) |
| `Filmback.SensorHorizontalOffset` / `SensorVerticalOffset` (mm) | `horizontalFilmOffset` / `verticalFilmOffset` (inches, `/ 25.4`) | adapted (unit) |
| `ConstrainAspectRatio` + `AspectRatio` (= plate crop aspect, else filmback aspect) + the constraint axis | `filmFit` | adapted (semantics) |
| `aperture_resolution` (the pixel extent of the film aperture inside the output resolution) | `defaultResolution`, camera `displayResolution` / `displayGateMask` | adapted |
| `output_resolution` (the full render gate) | reported only; the letterbox between it and the aperture is a documented difference | reported |
| `CurrentAperture` (f-stop) | `fStop` | direct |
| `FocusSettings.ManualFocusDistance` (cm), evaluated `DepthOfFieldFocalDistance` | `focusDistance` (cm) | direct |
| focus method other than `Disable` | `depthOfField` enabled | adapted |
| `LensSettings.SqueezeFactor` | `lensSqueezeRatio` | direct |
| evaluated `DepthOfFieldFstop` / `DepthOfFieldSensorWidth` | same Maya attributes as above | adapted (single Maya DOF model) |
| resolved perspective near clip (`GetFinalPerspectiveNearClipPlane`, with `near_clip_source` naming the source) | `nearClipPlane` | direct (Maya rejects both zero and large values, so the resolved engine value is what is transferred) |
| far clip (unbounded in Unreal) | `farClipPlane` | not representable (substituted, reported) |
| `Overscan`, `OverscanResolutionFraction`, asymmetric overscan | — | not representable |
| `CropSettings.AspectRatio` | resolution gate aspect | adapted (gate, not a camera attribute) |
| extra DOF shaping (blade count, Petzval bokeh, blur radius/amount, transition regions, occlusion) | — | not representable |
| `OffCenterProjectionOffset` | film offset | adapted (semantics) |
| `CurrentHorizontalFOV` / `CurrentVerticalFOV` | reported for checking only; Maya derives its angles from aperture and focal length | reported |

`direct` means the same physical quantity and unit after conversion. `adapted`
means the value survives but the host expresses it differently. `not
representable` means Maya cannot express it through camera attributes, so the
prototype reports the difference instead of approximating silently.

Resolution gate: Maya has one gate, while Unreal renders the film aperture
inside a possibly larger output resolution. Maya therefore writes
`aperture_resolution` (the pixels Unreal actually fills) to its
`defaultResolution`, and the client reports the full `output_resolution`
separately. Sending the full gate instead is what makes a 4:3 film aperture in
a 16:9 gate frame differently in Maya: Maya derives the axis it does not keep
from the gate aspect, so a mismatched gate changes the field of view. The
cross-host check measured this failure (0.25–0.38 NDC on the vertical axis)
before the aperture rectangle was transferred.

Film fit: Unreal fixes the field of view on the constraint axis
(`MaintainXFOV` keeps horizontal, `MaintainYFOV` keeps vertical) and derives the
other axis from the rendered aspect. A Cine Camera constrains its aspect ratio
to the plate crop aspect when the crop is enabled, otherwise to the filmback
aspect (`SensorAspectRatio * SqueezeFactor`), and renders that aperture inside
the output resolution with bars where the two aspects differ. Maya's
`Horizontal` and `Vertical` film fit keep the same axis fixed; the prototype
therefore set the Maya resolution gate to the pixel extent of the aperture
rectangle Unreal reports (`projection.view_rect`) and reports any Maya aspect
that cannot be expressed.

## Markers

A marker is an actor tagged `MtoUCameraSyncMarker`. Unreal reports each
marker's world location in centimetres and its normalized device coordinates
(NDC, `-1..1`, `+Y` up) computed from the evaluated camera's own
view-projection matrix. **NDC spans the film aperture, not the full output
resolution**, so a letterboxed render still compares like for like. `pixel` is
the same position inside `projection.view_rect` (the aperture rectangle).

Maya converts the same world location, projects it with the synced camera using
its own film-aperture math, and reports the delta per marker. A delta larger
than the recorded tolerance is a prototype failure, not a note.

## Camera cuts and subsequences

- The independent-player test uses `ULevelSequencePlayer::GetActiveCameraComponent`.
  The editor mode reads the open Sequencer's root time and
  `ISequencer::GetLastEvaluatedCameraCut()`, including when a shot is focused.
  Neither mode reimplements cut resolution.
- `camera_cut.stage` reports `root` when the active cut belongs to the root
  sequence, `subsequence` when the engine took it from a subsequence instance,
  and `pending` before the first frame.
- Maya receives only the resolved camera; it does not evaluate Sequencer data.
