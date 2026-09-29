# Issue #52: Unreal to Maya camera and Sequencer verification

Date: 2026-09-28–2026-09-29. The bounded camera prototype was verified on stock
Unreal 5.7.4 and Maya 2024. The follow-up uses the **open Level Sequence
editor** for its time and evaluated camera rather than a second player, and the
latest round separates the **evaluation identity** from the transport serial so
a paused target is not invalidated by its own heartbeats. A keyed Maya joint
pose witness returns on the prototype channel and is applied to a disposable
Unreal actor only when its session, generation, camera and evaluation time
match the target that is current when the report is read. This establishes a
small paused-frame loop with a convergence condition. It does not establish
production MtoULiveLink pose streaming, full-character same-frame application,
or continuous-playback performance. No product v9 code or package was changed.

## Current conclusion

| Area | Verified result |
| --- | --- |
| Official capability | Autodesk Unreal Live Link for Maya publishes Maya → Unreal camera data under MIT and its latest available integration targets UE 5.5. Epic's Camera Sync also moves the Maya viewport camera into Unreal. Neither supplies a complete Unreal → Maya camera path. Sources and the reuse/adapt/missing split remain in the prototype's `official-capabilities.md`. |
| Camera and framing | The engine's evaluated perspective Cine Camera, cut selection, film parameters and aperture extent reach one Maya camera. Known-marker disagreement is at most **1.09e-07 NDC**. The earlier Arnold check placed compared markers within **0.25 px** of prediction against a 2 px tolerance; that render was not repeated in this round. |
| Real editor time | The open editor's global/root time and `ISequencer::GetLastEvaluatedCameraCut()` drive publication, and one forced evaluation keeps the resolved camera in step with a moved playhead before a report is judged. Paused seeks 1001 → 1030 change the camera 50 → 85 mm. Focusing a subsequence retains root time 1030 and the evaluated camera. Stopping follow does not move the editor playhead. |
| Maya time | A chosen Maya origin maps to the Unreal playback start. By default both origins are the Unreal start, independent of Maya's connection frame. Maya 2024 evaluates fractional time directly: the host check reaches 1002.5 and a keyed joint between 1002 and 1003 reads 5.0. On stop, Maya restores its original connection frame, camera cleanup, playback range and existing keys. |
| Evaluation identity (new) | Prototype protocol v2 publishes `eval_serial` and `eval_identity` (`<sequence>@<tick>/<camera path>`) beside the transport `frame_serial`. Only a real jump, cut, shot or camera change starts a new generation; heartbeats repeat the current one. |
| Paused pose witness (new semantics) | Unreal samples the target, then reads client reports, then publishes, so a report is judged against the target that is current *now* rather than against the publication it answered. `EvalIdentityConvergence` proves: a slow report for the still-current target pairs after a heartbeat superseded its publication; a fresh, unpaired report that arrives in the same tick that left the target is refused before the new frame is published; replaying a publication never pairs twice; and a former connection's report is refused although every other identity field is current. |
| Convergence (new) | Acceptance is no longer "paired at least once": after the timeline stops, the target that is current must be answered within a bounded wait. The real session converged (`eval_serial == converged_paired_eval_serial == 2`), and the real `RealMayaPeer` test asserts the same condition. |
| Transport correction | The former apparent “Maya trailing bytes” were produced by the Unreal receiver treating a length-delimited UTF-8 conversion as a zero-terminated C string. Explicit-length construction removed the symptom; this round again recorded **0 anomalous lines, 0 discarded bytes, 0 failed sends**, and a deliberately malformed non-ASCII suffix verifies byte accounting (one character, two UTF-8 bytes). This round also bounds per-tick client work (64 lines, remainder kept for the next tick) and the report evidence queue (512 entries, evictions counted). |
| Close detection (new) | The engine reports a cleanly closed socket as connected while there are no pending bytes, so an idle client that left stayed registered. The session now probes readability with nothing to read — the stream end-of-file — before concluding the client is still there; the reconnect tests depend on this. |

## Scope and evidence

The prototype stays separate from MtoULiveLink: independent TCP port 54330,
editor-only Unreal plugin, no new external dependency, no change to product
protocol v9, no product packaging. The opt-in pose witness is one keyed Maya
joint's `translateX`, carried in the camera prototype's `applied` report and
mapped to a disposable Unreal actor's Y position. It validates the identity join
and time evaluation, not the product's full character-pose transport.

| Check | Result |
| --- | --- |
| Unreal build: `Build.bat UnrealEditor Win64 Development <repo>/unreal/ToolsLab.uproject -WaitMutex -NoHotReloadFromIDE` | Passed on UE 5.7.4. The target also needed a unity-build fix in the sibling multi-subject prototype, where two translation units declared the same anonymous-namespace log constant. |
| Unreal Automation: `Automation RunTests MtoUCameraSyncPrototype` | 7/7 passed: `CameraPayload`, `CameraCutsAndTime`, `SubsequenceTime`, `EditorSequencer`, `EvalIdentityConvergence`, `TrailingByteCount`, `RealMayaPeer`. |
| Real peer session: `-MtoUCameraSyncMayapy=`, `-MtoUCameraSyncPeer=`, `-MtoUEvidence=` | `RealMayaPeer` passed with the mayapy peer: peer exit code 0, `ok: true`, `phase: done`, no problems; 7 published frames, 6 applied, 5 pose reports paired, 1 superseded in-flight report refused at the cut, 2 synthetic replays refused (duplicate publication, former connection); converged; witness value 9; marker delta ≤ 1.0887e-07 NDC; 0 anomalous lines, 0 discarded bytes, 0 failed sends. |
| Maya mapping: `python -m unittest discover -s <prototype Maya tests> -t <prototype Maya root>` (and the same under mayapy) | 63/63 passed in both interpreters, including the new evaluation-identity validation cases. |
| Maya host: `mayapy maya_host_camera_tests.py --result <scratch JSON>` | 56/56 passed, including the echo of `eval_serial`/`eval_identity` in direct and socket reports, fractional keyed-joint evaluation, rejected payload rollback, no animation key or range changes, and time/camera restoration. |
| Repository gates: `python tools/validate_repository.py`; `python -m unittest discover -s tests` | Validation passed without warnings; 22/22 tool tests passed (1 skipped). |

The Unreal test uses the actual Level Sequence editor API, including a real
asset-editor toolkit in the test host. Its six-frame session is a pair of paused
positions with heartbeats and a camera cut. The refused in-flight report is the
expected outcome of the timeline leaving its target; the witness actor keeps the
value of the converged generation. No continuous-playback latency percentile or
drop budget has been measured.

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
Maya as pose authority, one active perspective camera, an explicit time origin,
exact subframe evaluation, and an identity-checked reply. Each data kind has one
writer; the overall system can carry data in both directions. The prototype's
second port does not establish integration with MtoULiveLink's existing
Maya → UE pose channel, and the single-joint witness on the prototype channel
does **not** substitute for the next step.

The next stage is a real C01 paused-positioning integration on an isolated copy
of the supplied `Backups.uproject`: confirm skeleton pairing and scene
dependencies, then prove that the full skeleton and Morphs apply through the
**product** pose path while the camera prototype drives Maya's time from the
editor Sequencer. That requires the product's pose data to be associated with
the same evaluation, which is explicit protocol design work — protocol version
or capability negotiation, both adapters, and conformance cases — because
product v9 carries no evaluation identity. Until that exists, no claim of
"same frame at any moment" or product readiness is made, and continuous
playback (latency percentiles, drop/hold policy) stays out of scope. Reconnect
needs a full Maya/product-client recovery test; this round proved only that a
former connection's report is refused and that a fresh connection starts
unpaired. Cached Playback remains off: ADR 0013 uses its own monotonic replay
clock and must be revised before it can share Sequencer time. No production
time-authority choice, persistent Maya-camera policy, MRQ output-resolution
authority or exit-frame policy was made here.

The original official-capability, camera-parameter and projection acceptance
remains valid within this bounded scope. The real-editor, evaluation-identity
and convergence work strengthens it, but it does not turn #52 into a complete
camera or animation product.
