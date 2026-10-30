# Measured mapping and remaining differences

Every number here was produced by the prototype on Unreal 5.7.4 and Maya 2024
on one machine. The Unreal values are what the engine evaluated; the Maya values
are what the node actually holds after the frame was applied, read back from
Maya. Nothing in this table is copied from documentation.

## Session used for the cross-host numbers

- Level Sequence at 24 fps with a non-zero playback start (1001), two camera
  cuts (a 50 mm f/2.0 camera, then an 85 mm f/4.0 camera from frame 1026), three
  tagged markers, and an output resolution of 1920×1080.
- Maya scene in centimetres at 24 fps; the explicit Maya origin is frame 1001.
- Six camera frames published and applied. The editor mode uses the open
  Sequencer's root time and evaluated cut; the Maya peer returns a keyed-joint
  pose witness at each applied time. A stale witness is refused after a cut.

## Per-parameter result

| Parameter | Unreal evaluated value | Maya value after apply | Class | Delta |
| --- | --- | --- | --- | --- |
| World transform | view location and basis of the active camera, cut 1 then cut 2 | world matrix written with `xform`, read back equal | direct | 0 |
| Focal length | 50 mm → 85 mm at the cut | 50 mm → 85 mm | direct | 0 |
| Film aperture (sensor 24.96 × 18.72 mm) | 24.96 / 18.72 mm | 0.982677 / 0.737008 in | adapted (unit) | 0 |
| Film offsets | 0 mm | 0 in | adapted (unit) | 0 |
| Film fit | constraint axis `MaintainXFOV` | `filmFit` 1 (`Horizontal`) | adapted (semantics) | — |
| Resolution gate | aperture 1440 × 1080 inside a 1920 × 1080 gate | `defaultResolution` 1440 × 1080 with the matching device aspect | adapted | — |
| Aperture | f/2.0 → f/4.0 | 2.0 → 4.0 | direct | 0 |
| Focus distance | 400 cm → 800 cm (manual focus) | 400 cm → 800 cm | direct | 0 |
| Depth of field | enabled, evaluated `DepthOfFieldFstop` equals the aperture | `depthOfField` on, same f-stop | adapted (one Maya DOF model) | 0 |
| Near clip | resolved engine default 10 cm (`near_clip_source: engine_default`) | `nearClipPlane` 10 cm | direct | 0 |
| Far clip | none in Unreal | 100000 cm, marked `far_clip_substituted`, source reported | not representable | substituted |
| Playback time | display frame 1001 → 1030 | Maya frame 1001 → 1030 | direct at equal rates | 0 |
| Differing-rate subframe | fixture maps a fractional display time to Maya 1002.5 | Maya `currentTime` reads 1002.5; keyed joint between 1002 and 1003 evaluates to 5.0 | adapted (explicit origins) | no rounding |
| Marker framing | Unreal NDC from the evaluated projection | Maya NDC from its own film-aperture projection | checked | ≤ 1.09e-07 NDC |

Marker comparison in the last session: centre `(0, 0)`, lower right
`(-2.1e-08, 3.7e-08)`, upper left `(2.9e-08, -3.8e-08)`.

## Framing evidence beyond the analytic comparison

- Unreal side: the aperture rectangle is 1620 × 1080 inside a 1920 × 1080 gate
  for a 36 × 24 mm sensor at 35 mm, a horizontal field of view of 2·atan(36/(2·35))
  = 54.43°, and a 5 mm sensor offset moves the optical-axis marker to NDC
  −0.2778, which is `2·5/36` as the engine documents.
- Maya side: rendering the synced camera with Arnold (`mtoa` 5.3.4.1) and
  locating the marker spheres in the image puts every compared marker within
  0.25 px of the position both hosts predict, against a 2 px tolerance, for the
  `Horizontal`, `Vertical`, and both `Fill` gate configurations. Markers a
  variant framing pushes out of frame are reported as skipped, not matched.
- Maya host checks: 54 headless checks pass, including the read-back world matrix, the
  resolution gate, the applied time, that no keys appear, that the playback
  range is untouched, that a frame missing a field is rejected without touching
  the camera and still reaches the report queue, and that `stop()` restores the
  previous time and removes the camera it created.

## Differences the prototype reports instead of hiding

| Difference | Evidence |
| --- | --- |
| Maya has one gate; Unreal renders an aperture inside a larger output resolution. The prototype writes the aperture extent to Maya and reports the full gate separately. Sending the full gate makes a 4:3 aperture in a 16:9 gate frame differently (measured 0.25–0.38 NDC error on the vertical axis) because Maya derives the unkept axis from the gate aspect. |
| Unreal has no far clip for perspective cameras; Maya always needs one. The substituted value and its source are reported per frame. |
| Unreal overscan (uniform, asymmetric, resolution fraction) has no Maya camera equivalent. |
| Extra depth-of-field shaping (blade count, Petzval bokeh, blur radius/amount, transition regions, occlusion) has no Maya camera equivalent. Parameter equality was checked; image equality was not, and no claim is made. |
| Unreal's unbounded `CropSettings` and `OffCenterProjectionOffset` survive only through the gate aspect and the film offsets; the prototype reports both rather than approximating. |
| The earlier apparent client suffix was caused by the Unreal receiver reading beyond a length-delimited UTF-8 conversion. Explicit-length string construction eliminates it: the later real editor/Maya session records zero anomalous lines, zero discarded bytes and zero failed sends. The former `28112` was a character count mislabeled as bytes; its exact raw byte count was not recorded. |
| Maya cannot express camera cuts or shot selection; only the resolved camera crosses the wire, and `camera_cut.stage` tells whether the engine took it from the root sequence or a subsequence. |

## What was not verified

- Depth-of-field image equality between the two renderers: only parameter
  equality and the Maya render were produced. Epic documents its own cinematic
  depth of field as a procedural bokeh that does not change light intensity, so
  no pixel equivalence is claimed.
- Multi-camera sessions, nested subsequences inside a subsequence, orthographic
  cameras, and Movie Render Pipeline output-resolution discovery.
- Product MtoULiveLink pose-channel integration, full-character pose application,
  continuous editor playback latency, and a deliberate drop policy. The keyed
  joint witness exercises identity matching on the prototype channel only.
- Any Autodesk or Epic plugin installation; the prototype depends on neither.
