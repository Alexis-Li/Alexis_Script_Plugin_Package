# Issue #52: Unreal to Maya camera and Sequencer verification

Date: 2026-09-28. The bounded camera prototype was verified on stock Unreal
5.7.4 and Maya 2024. The follow-up now uses the **open Level Sequence editor**
for its time and evaluated camera, rather than a second player. A keyed Maya
joint pose witness returns on the prototype channel and is applied to a
disposable Unreal actor only when its session, frame serial, camera and
evaluation time match. This establishes a small paused-frame loop. It does not
establish production MtoULiveLink pose streaming or continuous-playback
performance. No product v9 code or package was changed.

## Current conclusion

| Area | Verified result |
| --- | --- |
| Official capability | Autodesk Unreal Live Link for Maya publishes Maya → Unreal camera data under MIT and its latest available integration targets UE 5.5. Epic's Camera Sync also moves the Maya viewport camera into Unreal. Neither supplies a complete Unreal → Maya camera path. Sources and the reuse/adapt/missing split remain in the prototype's `official-capabilities.md`. |
| Camera and framing | The engine's evaluated perspective Cine Camera, cut selection, film parameters and aperture extent reach one Maya camera. Known-marker disagreement is at most **1.09e-07 NDC**. The earlier Arnold check placed compared markers within **0.25 px** of prediction against 2 px tolerance; that render was not repeated in this follow-up. |
| Real editor time | The open editor's global/root time and `ISequencer::GetLastEvaluatedCameraCut()` drive publication. Paused seeks 1001 → 1030 change the camera 50 → 85 mm. Focusing a subsequence retains root time 1030 and the evaluated camera. Stopping follow does not move the editor playhead. |
| Maya time | A chosen Maya origin maps to the Unreal playback start. By default both origins are the Unreal start, independent of Maya's connection frame. Maya 2024 evaluates fractional time directly: the host check reaches 1002.5 and a keyed joint between 1002 and 1003 reads 5.0. On stop, Maya restores its original connection frame, camera cleanup, playback range and existing keys. |
| Paused pose witness | In the last real editor/Maya session, six camera frames produced six intact application reports; five witnesses paired and one was refused because the mid-session camera cut superseded its report while it was in flight (the previous identical run paired all six). The Unreal test actor ended at the mapped witness value 9 in both runs. A replayed old report and the same report after a new TCP connection were both refused without moving it. The second connection used a new session identity. |
| Transport correction | The former apparent “Maya trailing bytes” were produced by the Unreal receiver treating a length-delimited UTF-8 conversion as a zero-terminated C string. It read beyond the converted buffer. Explicit-length construction removes the symptom: last real session **0 anomalous lines, 0 discarded bytes, 0 failed sends**. The old 28112 figure was counted in characters and mislabeled as bytes; the original raw byte count was not preserved. A deliberate non-ASCII suffix now verifies byte accounting (one character, two UTF-8 bytes). |

## Scope and evidence

The prototype stays separate from MtoULiveLink: independent TCP port 54330,
editor-only Unreal plugin, no new external dependency, no change to product
protocol v9, no product packaging. The opt-in pose witness is one keyed Maya
joint's `translateX`, carried in the camera prototype's `applied` report and
mapped to a disposable Unreal actor's Y position. It validates the identity
join and time evaluation, not the product's full character-pose transport.

| Check | Result |
| --- | --- |
| Unreal build: `Build.bat UnrealEditor Win64 Development <repo>/unreal/ToolsLab.uproject -WaitMutex -NoHotReloadFromIDE` | Passed on UE 5.7.4. |
| Unreal Automation: `Automation RunTests MtoUCameraSyncPrototype` with `-MtoUCameraSyncMayapy=`, `-MtoUCameraSyncPeer=`, and `-MtoUEvidence=` | 6/6 passed, including real editor + Maya, focused shot, reconnect and malformed UTF-8 byte-count check. Unreal's command-line exit code alone was not used; the Automation log reports all six successes. |
| Maya mapping: `python -m unittest discover -s <prototype Maya tests> -t <prototype Maya root>` (and the same command under mayapy) | 61/61 passed in both interpreters. |
| Maya host: `mayapy maya_host_camera_tests.py --result <scratch JSON>` | 54/54 passed, including unrelated connection frame, fractional keyed-joint evaluation, rejected payload rollback, no animation key or range changes, and time/camera restoration. |
| Real-session result JSON | Maya `ok: true`, `phase: done`, no problems; six application reports applied, five pose witnesses paired plus one refusal at the cut jump, two **synthetic** stale/old-session replays refused, 0 anomalous client lines, 0 discarded bytes, 0 failed sends; marker delta ≤ 1.09e-07 NDC. |
| Repository gates: `python tools/validate_repository.py`; `python -m unittest discover -s tests -v` | Validation passed without warnings; 22/22 tool tests passed. |

The Unreal test uses the actual Level Sequence editor API, including a real
asset-editor toolkit in the test host. Its six-frame session is a pair of
paused positions with heartbeats and a camera cut. The quick jump can supersede
a pose report that is still in flight: the two final runs paired six and five
of six, and the refused report was the one the cut had already replaced, so the
witness actor kept the value of the latest published frame. The contract
refuses a pose when its serial, camera, time or session no longer matches the
latest published frame, so a slow client cannot overwrite the current witness.
No continuous-playback latency percentile or drop budget has been measured.

## Mapping and representational limits

The detailed [mapping record](../../../composite/MtoULiveLink/prototypes/camera-sync/mapping.md)
classifies world transform, focal length, filmback, offsets, film fit, crop,
aperture, focus, depth of field, clip planes, output/aperture resolution and
known-marker projection. The Maya resolution gate must use Unreal's **aperture
pixel extent**, not the full output gate; otherwise a 4:3 aperture inside a
16:9 output differs by 0.25–0.38 NDC in the measured setup. Far clip,
overscan, extra bokeh shaping and Maya-side camera-cut concepts cannot be
expressed equivalently. Depth-of-field parameters were compared, not
cross-renderer image equality. Movie Render Pipeline resolution discovery,
orthographic and simultaneous multi-camera sessions remain outside scope.

## Product boundary and remaining acceptance

The smallest plausible product would keep UE as camera and time authority,
Maya as pose authority, one active perspective camera, an explicit time
origin, exact subframe evaluation, and an identity-checked reply. Each data
kind has one writer; the overall system can carry data in both directions.
The prototype's second port does not establish integration with
MtoULiveLink's existing Maya → UE pose channel.

Before productization, a separate integration pass must send real character
poses through the product path with the same evaluation identity, verify
paused seeks on an actual bound character, then measure continuous editor
playback latency and define how to drop or hold late frames. Reconnect needs
a full Maya/product-client recovery test; this pass proved only that the
prototype publisher rejects a former session's replay. Cached Playback
remains off: ADR 0013 uses its own monotonic replay clock and must be revised
before it can share Sequencer time. No production time-authority choice,
persistent Maya-camera policy, MRQ output-resolution authority or exit-frame
policy was made here.

The original official-capability, camera-parameter and projection acceptance
remains valid within this bounded scope. The real-editor and pose-witness
follow-up strengthens it, but it does not turn #52 into a complete camera or
animation product.
