# Issue #52: Unreal to Maya camera and Sequencer verification

Date: 2026-09-28–2026-09-29. The bounded camera prototype was verified on stock
Unreal 5.7.4 and Maya 2024. The follow-up uses the **open Level Sequence
editor** for its time and evaluated camera rather than a second player. Two
review rounds then tightened the prototype: the **evaluation identity** is
separated from the transport serial so a paused target is not invalidated by its
own heartbeats, and it now covers the **published camera content** as well, so
editing the camera that is already selected at the sequence time that is already
sampled starts a new target. A keyed Maya joint pose witness returns on the
prototype channel and is applied to a disposable Unreal actor only when its
session, generation, camera and evaluation time match the target that is current
when the report is read. This establishes a small paused-frame loop with a
convergence condition. It does not establish production MtoULiveLink pose
streaming, full-character same-frame application, or continuous-playback
performance. No product v9 code or package was changed.

## Current conclusion

| Area | Verified result |
| --- | --- |
| Official capability | Autodesk Unreal Live Link for Maya publishes Maya → Unreal camera data under MIT and its latest available integration targets UE 5.5. Epic's Camera Sync also moves the Maya viewport camera into Unreal. Neither supplies a complete Unreal → Maya camera path. Sources and the reuse/adapt/missing split remain in the prototype's `official-capabilities.md`. |
| Camera and framing | The engine's evaluated perspective Cine Camera, cut selection, film parameters and aperture extent reach one Maya camera. Known-marker disagreement is at most **1.09e-07 NDC**. The earlier Arnold check placed compared markers within **0.25 px** of prediction against a 2 px tolerance; that render was not repeated in this round. |
| Real editor time | The open editor's global/root time and `ISequencer::GetLastEvaluatedCameraCut()` drive publication, and one forced evaluation keeps the resolved camera in step with a moved playhead before a report is judged. Paused seeks 1001 → 1030 change the camera 50 → 85 mm. Focusing a subsequence retains root time 1030 and the evaluated camera. Stopping follow does not move the editor playhead. |
| Maya time | A chosen Maya origin maps to the Unreal playback start. By default both origins are the Unreal start, independent of Maya's connection frame. Maya 2024 evaluates fractional time directly: the host check reaches 1002.5 and a keyed joint between 1002 and 1003 reads 5.0. On stop, Maya restores its original connection frame, camera cleanup, playback range and existing keys. |
| Evaluation identity | Prototype protocol v2 publishes `eval_serial` and `eval_identity` beside the transport `frame_serial`. The identity is `<sequence>@<tick>+<milli-tick>/<camera path>#<camera content>`: the sequence time at **1/1000 tick** precision plus a canonical text of every camera value the `frame` message publishes, so the identity distinguishes times inside one tick and treats a same-time edit of the evaluated camera (transform, focal length, filmback, offsets, depth of field) as a new target. Heartbeats repeat the current generation. |
| Paused pose witness | Unreal samples the target (time **and** camera content), then reads client reports, then publishes, so a report is judged against the target that is current *now* rather than against the publication it answered. `EvalIdentityConvergence` proves: a slow report for the still-current target pairs after a heartbeat superseded its publication; a fresh, unpaired report that arrives in the same tick that left the target is refused before the new frame is published; replaying a publication never pairs twice; and a former connection's report is refused although every other identity field is current. `CameraContentIdentity` adds: an unchanged camera rebuilds the same identity; editing the evaluated camera's focal length and transform at the same tick starts a new generation and refuses an in-flight report that answered the previous content; the report for the changed content is applied; a sub-tick move inside one tick is a new target with the same content digest; and a pose edited at the parked frame is applied on the next heartbeat of the unchanged target. |
| Convergence | Acceptance is no longer "paired at least once": after the timeline stops, the target that is current must be answered within a bounded wait. The real session converged (`eval_serial == converged_paired_eval_serial == 2`), and the real `RealMayaPeer` test asserts the same condition. |
| Transport correction | The former apparent “Maya trailing bytes” were produced by the Unreal receiver treating a length-delimited UTF-8 conversion as a zero-terminated C string. Explicit-length construction removed the symptom; this round again recorded **0 anomalous lines, 0 discarded bytes, 0 failed sends**, and a deliberately malformed non-ASCII suffix verifies byte accounting (one character, two UTF-8 bytes). |
| Transport bounds | Per pump the session drains the complete lines it has already buffered **before** reading the socket, receives at most 256 KiB and buffers at most one message plus one 4 KiB read, so a burst larger than the 64-line budget keeps draining after the sender goes silent. A message that exceeds 64 KiB without a complete line fails the session closed with `CLIENT_MESSAGE_TOO_LARGE` instead of growing the buffer, a dropped connection discards its partial line, and the report evidence queue is capped at 512 entries with counted evictions. `ClientBufferBounds` covers a 65-line silent burst, a line split across two sends, the oversize refusal and a clean reconnect. |
| Connection ownership | The TCP listener runs on its own thread, and the prototype used to accept a connection and reset the input buffer from that thread while the editor thread was parsing, which a burst followed by a reconnect turned into a real out-of-range read during this round's check. The listener thread now only queues accepted sockets; the editor thread adopts them (new session identity, empty buffer) or refuses them with `SESSION_BUSY`, so no session state is shared across threads. |
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
| Unreal Automation: `Automation RunTests MtoUCameraSyncPrototype` | 9/9 passed: `CameraPayload`, `CameraCutsAndTime`, `SubsequenceTime`, `EditorSequencer`, `EvalIdentityConvergence`, `CameraContentIdentity`, `ClientBufferBounds`, `TrailingByteCount`, `RealMayaPeer`. |
| Real peer session: `-MtoUCameraSyncMayapy=`, `-MtoUCameraSyncPeer=`, `-MtoUEvidence=` | `RealMayaPeer` passed with the mayapy peer: peer exit code 0, `ok: true`, `phase: done`, no problems; 7 published frames, 6 applied (2 applied, 4 heartbeats), 5 pose reports paired, 1 superseded in-flight report refused at the cut, 2 synthetic replays refused (duplicate publication, former connection); converged (`eval_serial == converged_paired_eval_serial == 2`); witness value 9; marker delta ≤ 1.0887e-07 NDC; 0 anomalous lines, 0 discarded bytes, 0 failed sends. |
| Maya mapping: `python -m unittest discover -s <prototype Maya tests> -t <prototype Maya root>` (and the same under mayapy) | 63/63 passed in both interpreters. |
| Maya host: `mayapy maya_host_camera_tests.py --result <scratch JSON>` | 60/60 passed, including the echo of `eval_serial`/`eval_identity` in direct and socket reports, a pose keyed at the parked frame being reported by the next heartbeat, fractional keyed-joint evaluation, rejected payload rollback, no animation key or range changes, and time/camera restoration. |
| Repository gates: `python tools/validate_repository.py`; `python -m unittest discover -s tests` | Validation passed without warnings; 22/22 tool tests passed (1 skipped). |

The Unreal test uses the actual Level Sequence editor API, including a real
asset-editor toolkit in the test host. Its six-frame session is a pair of paused
positions with heartbeats and a camera cut. The refused in-flight report is the
expected outcome of the timeline leaving its target; the witness actor keeps the
value of the converged generation. `CameraContentIdentity` covers the same case
for a camera edited at the sequence time that is already sampled, and
`ClientBufferBounds` covers the transport bounds in isolation. No
continuous-playback latency percentile or drop budget has been measured.

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

The [2026-09-29 review, now consolidated on the issue](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/52#issuecomment-5888803268)
also identifies an unverified shutdown risk at the `ad3a85e` implementation:
`Stop()` drains `PendingClients` before stopping/joining the listener, which can
enqueue another socket in between. This is a source/lifecycle finding, not a
host-observed leak or crash. Stop and join the listener before final queue
cleanup, then exercise connection arrival during shutdown and repeated starts
and stops. The review reran 63 pure mapping tests and inspected prior host
reports; it did not rebuild or rerun the UE suite. This 2026-09-30 record update
does not add a host verification run.

The smallest plausible product would keep UE as camera and time authority,
Maya as pose authority, one active perspective camera, an explicit time origin,
exact subframe evaluation, and an identity-checked reply. Each data kind has one
writer; the overall system can carry data in both directions. The prototype's
second port does not establish integration with MtoULiveLink's existing
Maya → UE pose channel, and the single-joint witness on the prototype channel
does **not** substitute for it.

The next stage inside the prototype scope is a **natural editor-loop check**:
drag the playhead, switch cuts, close and reopen the sequence, disconnect and
reconnect by hand, and confirm the restoration of Maya's own current frame, the
temporary camera, the resolution settings and the callbacks. The automated
checks drive the real editor API but pump the session and force evaluations
themselves, so they do not replace that timing.

Product integration stays deferred and unified: after Issue #52's prototype
checks and the #53/#54 prototypes have produced reproducible real samples, the
decision on the shared coordinate and reference origin, the time-authority
switch, evaluation and session identity, object pairing, temporary-node
ownership, protocol version or capability negotiation, and the UI belongs to
Issue #47. The real C01 paused-positioning integration on an isolated copy of
the supplied `Backups.uproject` — confirming skeleton pairing and scene
dependencies, then applying the full skeleton and Morphs through the **product**
pose path — needs that design first, because product v9 carries no evaluation
identity. Until it exists, no claim of "same frame at any moment" or product
readiness is made, and continuous playback (latency percentiles, drop/hold
policy) stays out of scope. The prototype now carries the bounded-transport
bounds and the content-covered identity that such an integration would have to
reproduce. Reconnect needs a full Maya/product-client recovery test; this round
proved only that a former connection's report is refused, that the session
starts a new connection with an empty buffer, and that a fresh connection starts
unpaired. Cached Playback remains off: ADR 0013 uses its own monotonic replay
clock and must be revised before it can share Sequencer time. No production
time-authority choice, persistent Maya-camera policy, MRQ output-resolution
authority or exit-frame policy was made here.

The original official-capability, camera-parameter and projection acceptance
remains valid within this bounded scope. The real-editor, evaluation-identity
and convergence work strengthens it, but it does not turn #52 into a complete
camera or animation product.
