# Issue #52: Unreal to Maya camera and Sequencer verification

Date: 2026-09-28. A bounded two-host prototype was built and measured on Unreal
5.7.4 and Maya 2024. Unreal evaluates the camera and owns the time; Maya applies
the camera and follows. All prototype checks pass. Issue #52 stays open until
the product boundary and the pending user decisions below are answered; nothing
here is a commitment to a camera product, and no product code, protocol, or
package changed.

## What was delivered

| Area | Result |
| --- | --- |
| Official capability review | Autodesk's Unreal Live Link for Maya is Maya → Unreal only, MIT licensed, latest release targets UE 5.5; Epic's "Enable Camera Sync" pushes Maya's viewport camera into Unreal. No official Unreal → Maya camera path exists, so the direction had to be built. Reuse, adaptation, and missing pieces are recorded with sources in the prototype's `official-capabilities.md`. |
| Prototype | Editor-only Unreal publisher driven by a level sequence player, plus a Maya follower that owns one disposable camera, the timeline follow, and its own projection maths. Contract in `protocol.md`, measured mapping in `mapping.md`. |
| Cross-host session | Six frames over a real socket across two camera cuts: six applied, zero rejected, marker agreement ≤ 1.09e-07 NDC. |
| Unreal-only checks | Camera payload, camera cuts with a non-zero playback start, and a master sequence with a subsequence shot all pass. |
| Maya checks | 60 pure tests, 57 host checks in a disposable scene, and an Arnold render check of the synced framing (≤ 0.25 px against a 2 px tolerance). |

## Acceptance criteria

| Criterion | Outcome |
| --- | --- |
| Read the official implementations and list reuse, adaptation, missing parts, licence and version constraints; do not assume a reverse Camera Sync exists | Done. The reverse path does not exist officially; Autodesk's camera subject carries transform, f-stop, aspect, horizontal field of view, focal length, focus distance, projection mode and static film back; their Unreal side stops at UE 5.5 and nothing was installed or compiled. |
| Minimal prototype takes the current evaluated camera from Unreal and covers transform, focal length, horizontal/vertical field of view, film back, film fit and offset, crop, aperture, focus distance, depth of field, output resolution and the Maya resolution gate | Done for a perspective cinematic camera. Field of view is derived from focal length and film back on both hosts; crop reaches Maya as the aperture aspect; overscan and extra depth-of-field shaping are reported as not representable. |
| Validate the projected framing with known spatial markers, list the mapping and the differences, keep depth-of-field parameter equality separate from cross-renderer image equality, and report anything that cannot be expressed | Done. Markers agree to 1.09e-07 NDC analytically and within 0.25 px in a Maya render; every parameter is classified as direct, adapted, or not representable with the measured delta. Depth of field was verified at parameter level only, and no image-equality claim is made. |
| Verify Unreal as the single time authority, differing frame rates and a non-zero start, camera cuts and subsequence time conversion, no feedback loop; exiting restores the original time control and rewrites no animation asset | Done. Unreal drives the sequence player; a client time request is refused and recorded; the Maya frame follows the display frame across the cut and the query-mapped subsequence content; on exit Maya's previous current time returns, the created camera is removed, the playback range and every existing key are unchanged. |
| State the mutual exclusion with the MtoU real-time and cached workflows and the integration path; the prototype runs with MtoU cache off and does not depend on the cache ticket | Done, as a written strategy plus a structural property: the prototype owns a second, independent channel (port 54330 versus the product's 54321) and never writes Unreal time, so no loop can form. It was run with no MtoU session at all, which is the cache-off case. |
| Deliver a reproducible prototype, sample inputs, results and errors, and a support list; keep items unverified when a host is unavailable rather than passing documentation off as integration | Done for everything the two hosts could run on this machine; the unverified list is explicit (depth-of-field image equality, multi-camera sessions, orthographic cameras, Movie Render Pipeline resolution discovery, nested subsequences). |
| Propose a minimal product boundary, the existing decisions that need revision, and the pending user decisions; do not expand into a full camera product or require unverified dependencies | Proposed below. No new dependency was added and no product file was touched. |

## Evidence

| Check | Command shape | Result |
| --- | --- | --- |
| Unreal prototype suite | `UnrealEditor-Cmd.exe <ToolsLab.uproject> -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests MtoUCameraSyncPrototype" -TestExit="Automation Test Queue Empty"` | 4 of 4 tests pass: `CameraPayload`, `CameraCutsAndTime`, `SubsequenceTime`, `RealMayaPeer` |
| Cross-host session | Same host with `-MtoUCameraSyncMayapy=`, `-MtoUCameraSyncPeer=`, `-MtoUEvidence=` | Peer exit 0, `ok: true`, `phase: done`, no problems; 6 published, 6 applied, 0 rejected |
| Maya pure tests | `python -m unittest discover -s <maya prototype tests> -t <maya prototype root>` | 60 tests pass under CPython 3 and under mayapy 3.10 |
| Maya host checks | `mayapy maya_host_camera_tests.py --result <json> --render --render-dir <dir>` | 57 checks pass; Arnold render check passes for four gate configurations |
| Repository gates | `python tools/validate_repository.py`, `python -m unittest discover -s tests` | Pass |

The cross-host session reported one prototype-level anomaly: the Maya client
appended extra bytes after two of its messages (28112 bytes total). The
publisher reads the first complete JSON object per line, counts and reports the
discarded bytes, and no applied value changed. The same client is byte-clean
against a plain Python server and a non-Maya client is byte-clean against this
publisher, so the anomaly is recorded as a host-side limit rather than treated
as verified behaviour.

## Minimal product boundary proposed

- One active perspective camera resolved through the engine's camera cut
  evaluation, published to one Maya camera; the product keeps its own channel
  and does not extend the streaming protocol (v9).
- Transferred values: evaluated view transform, focal length, film back with
  sensor offsets, film fit, aperture, focus distance, depth-of-field
  enablement, near clip, the aperture pixel extent, and the full render gate.
- Unreal stays the only writer of the sequence time; Maya follows and restores
  its own time on exit.
- Explicitly outside the boundary: overscan, extra depth-of-field shaping,
  image-level equivalence, camera-cut concepts on the Maya side, multi-camera
  sessions, and resolution discovery from Movie Render Pipeline settings.

## Decisions that need revision

- The roadmap requires one time authority per mode; the verification shows the
  camera publisher must never write Unreal time, and that Cached Playback must
  be stopped (or its own authority defined) before a Maya timeline follow is
  meaningful. That rule belongs in a decision record alongside ADR 0013 rather
  than only in a prototype.
- Maya's single resolution gate against Unreal's aperture-inside-gate rendering
  needs a stated product rule: the product must transfer the aperture extent,
  not the output resolution, or framing will differ.
- The product's current real-time preview streams Maya poses to Unreal while
  this direction streams Unreal camera and time to Maya. Their combination is
  coherent only if Maya's followed time is the same time the poses are sampled
  at; the integration plan must say so explicitly.

## Pending user decisions

1. Whether linked camera preview should make Unreal the time authority at all,
   or whether Maya should stay the master and only the camera follow.
2. Whether the Maya camera should be a persistent, named scene node owned by
   the workflow, or a transient camera like the prototype's.
3. Which output resolution is authoritative for the product: the Movie Render
   Pipeline target, the backbuffer with overscan, or an explicit project value.
4. Whether leaving a linked session should restore Maya's previous current time
   (prototype behaviour) or keep the followed frame.

## Delivered files

- `composite/MtoULiveLink/prototypes/camera-sync/` — protocol, official
  capability review, measured mapping, Unreal plugin, Maya follower, tests, and
  the cross-host peer.
- `unreal/ToolsLab.uproject` — the prototype plugin directory and its enable
  entry, so the documented host project builds it.
