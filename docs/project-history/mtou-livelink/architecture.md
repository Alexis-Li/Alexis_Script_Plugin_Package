# MtoU_LiveLink Architecture

Date: 2026-07-31
Status: Version 0.4.0 implemented and locally verified; unreleased
Production acceptance baseline: Stock Unreal Editor 5.7.4 completed 2026-08-11
Current production acceptance fixture: C01 animation/binding/Clothes 09 export;
pending a complete rerun
Last aligned with implementation: 2026-08-29

## Summary

MtoU_LiveLink provides a local, real-time character animation preview from
Autodesk Maya 2022 to Unreal Engine 5.7 Editor. It streams the evaluated
deformation skeleton and BlendShape values from one Maya character into one
Unreal Live Link subject. A user-created binding asset references an existing
Unreal Skeletal Mesh and can be dragged into a level. The placed actor keeps its
Unreal world transform, so the actor location becomes the Unreal equivalent of
the Maya origin.

The implementation is a thin bridge: Maya uses Python and the standard library
to send data over localhost TCP, while Unreal converts the received data into
native Live Link animation frames. It does not copy or port Autodesk's LiveLink
source code and adds no third-party production dependencies.

## Existing Production Workflow

The existing FBX workflow remains unchanged:

1. `Input.mel` selects `root` and its hierarchy together with `HighMesh`,
   disconnects rig inputs, moves the export objects to world, deletes the rig
   group, and exports the character FBX.
2. That FBX is renamed and imported into Unreal as the Skeletal Mesh and
   Skeleton used by the project.
3. Animation is authored in a Maya scene that references or proxies the rig.
   Controllers, IK, and constraints drive the deformation skeleton.
4. `Ani_Export{一键fbx导出}.mel` currently merges namespaces, bakes the
   evaluated animation onto `root` and BlendShapes, and exports an animation
   FBX for manual import into Unreal.

MtoU_LiveLink reads the same evaluated deformation skeleton that the animation
export script bakes. Unreal never needs to understand Maya controls or
constraints. The Live Link tool must not execute, modify, or replace either MEL
script, and it must not disconnect attributes, merge namespaces, reparent
objects, delete groups, bake animation, or export FBX.

## Current Production Acceptance Fixture

The production fixture selected on 2026-08-20 consists of:

- `C01_Body_IdleStand02_ChangeClothes.ma`, the Maya 2022 animation scene at
  30 fps with playback frames 0 through 320 inclusive;
- `SK_C01.ma`, the binding scene referenced by that animation scene as
  `SK_C01RN` in the `SK_C01` namespace;
- `SK_C01_Clothes_09.fbx`, the skeleton-and-mesh export from the binding scene
  used to create the Unreal Skeletal Mesh.

Acceptance resolves the animation scene's reference to the supplied binding
scene without saving either Maya file, imports the unchanged FBX into Unreal,
and exercises the negotiated character scene over all 321 display frames. The
fixture is external test data and must not be copied into the repository or a
release package. The stock-engine C04 acceptance remains the historical
baseline until this C01 fixture completes the same gate; its bone counts, frame
sizes, timings, and latency measurements are not C01 claims.

## Goals

- Preview one Maya character in Unreal Editor in real time.
- Support evaluated joint transforms produced by controls, IK, constraints,
  referenced rigs, and proxy rigs.
- Stream matching BlendShape values as Unreal Live Link curves.
- Update while posing, editing the current frame, playing, or scrubbing Maya's
  timeline.
- Bind to an existing Unreal Skeletal Mesh derived from the same Maya
  deformation skeleton.
- Let the user choose the Content Browser folder and asset name for the binding.
- Complete the final link by dragging the binding asset into an Unreal level.
- Preserve each placed actor's Unreal world transform as the Maya-origin offset.
- Support production characters with 700 or more bones without an artificial
  application-level bone, curve, or message-size limit.
- Fail clearly and cleanly when selection, transport, or skeleton validation
  fails.

## Non-goals

- Exporting, importing, or reimporting the character FBX.
- Persisting streamed frames or creating an Animation Sequence.
- Replacing the existing animation FBX export workflow.
- Multiple simultaneous Maya characters.
- Retargeting between different bone hierarchies or partial skeleton matching.
- Cross-machine or packaged-game streaming.
- Cameras, lights, props, materials, textures, mesh topology, or rig topology.
- Automatic reconnect after either host restarts.
- Bidirectional timeline control.

These capabilities can be designed separately if the first version proves
useful. No abstractions or configuration are added for them now.

## Supported Environment

- Windows 64-bit only.
- Autodesk Maya 2022.4, verified on the current workstation.
- Stock Unreal Engine 5.7.4, verified on the current workstation.
- Topia Engine 5.7.4, with Win64 plugin compilation and Athena editor loading
  verified on the current workstation.
- Unreal Editor only.

Stock UE 5.7.4 remains the full functional-acceptance baseline. The separate
Topia record covers compilation and editor loading without engine or host-project
source/configuration edits; it does not extend the stock end-to-end behavior
claim to every third-party UE 5.7 build.

## Names and Repository Locations

The user-facing product and Unreal `FriendlyName` are `MtoU_LiveLink`.
Technical identifiers omit the underscore to satisfy repository and Unreal
module conventions:

- Maya tool: `composite/MtoULiveLink/maya/MtoULiveLink/`
- Unreal plugin: `composite/MtoULiveLink/unreal/MtoULiveLink/`
- Runtime module: `MtoULiveLink`
- Editor module: `MtoULiveLinkEditor`

Both components share product version `0.4.0`. The composite project root owns
the matching English and Chinese READMEs, changelog, and license; host-specific
tests stay beside the implementation they exercise.

## Architecture

### Maya tool

The Maya side is one directly runnable Python file:

`composite/MtoULiveLink/maya/MtoULiveLink/scripts/MtoULiveLink.py`

It owns:

- the small native Maya UI;
- a character-scene module that owns deformation-root and Display validation,
  joint hierarchy and BlendShape discovery, namespace normalization,
  Maya-to-Unreal transform conversion, pose sampling, and its Maya callbacks;
- a streaming-session module that owns one connection attempt, its sender
  worker, sampling timer, connection-lifetime callbacks, fixed character
  revision, state transitions, and idempotent cleanup;
- compact JSON message creation;
- a single localhost TCP client and sender worker.

The character-scene module presents one small interface: capture a completely
configured character, read its immutable snapshot, sample a frame at the same
snapshot revision, and close it idempotently. It emits semantic character
change events rather than exposing Maya callback arguments. Display outfit
changes pause the current revision immediately, coalesce Maya notifications,
and atomically publish the refreshed snapshot after deferred scene evaluation.
The UI controller supplies explicit user selections and renders results; it
does not own DAG handles, curve plugs, character callbacks, or subject records.

Each streaming session borrows one character scene and exposes only start,
frame-rate change, and stop operations. Starting is transactional: a session is
returned only after its worker, scene callbacks, and sampling timer are all
active. It reports ready, failed, and stopped outcomes as semantic events,
defers failure cleanup out of Maya timer callbacks, and preserves the first
terminal outcome when failure and user stop race. Scene sampling or revision
failures require a fresh character capture; transport failures leave the
configured character scene reusable. The UI controller renders these events
but does not own worker, timer, connection callback, or negotiation state.

The file exposes `run()` and declares the sole Maya runtime version. The
character-scene interface is the test surface for scene discovery, sampling,
refresh, invalidation, and cleanup. The streaming-session interface is the test
surface for startup rollback, negotiation state, sampling handoff, frame-rate
changes, terminal races, and cleanup. Protocol and mathematical conversion
helpers remain directly testable outside Maya, while host integration tests
exercise both interfaces in Maya 2022.

### Unreal plugin

The Runtime module owns:

- the localhost listener and single accepted Maya connection;
- protocol parsing and validation;
- pure connection negotiation between captured Maya character facts and Unreal
  target facts, including complete skeleton mapping and Morph differences;
- the automatically registered native Live Link source;
- the fixed `MtoU_Character` Live Link subject;
- `UMtoULiveLinkBinding`, which references one existing Skeletal Mesh;
- `AMtoULiveLinkActor`, which owns a Skeletal Mesh Component and applies the
  subject through Unreal's native `ULiveLinkInstance`.

The Editor module owns:

- the Content Browser factory for `MtoU_LiveLink Binding` assets;
- the actor factory that turns a binding asset dragged into a viewport into an
  `AMtoULiveLinkActor`;
- editor-only status presentation and asset validation.

The plugin contains code only. Binding assets created by users live under their
chosen project `/Game/...` folders and reference, rather than copy, the selected
Skeletal Mesh.

## User Workflow

1. Enable MtoU_LiveLink and Unreal's Live Link dependencies, then restart the
   editor.
2. In the desired Content Browser folder, create and name an
   `MtoU_LiveLink Binding` asset.
3. Open the binding and select the Skeletal Mesh imported through the existing
   FBX workflow.
4. Drag the binding into the level and place the resulting actor wherever the
   Maya origin should appear.
5. In the Maya animation scene, run `MtoULiveLink.py`, select the deformation
   skeleton root (including a namespaced root such as `Hero:root`), and click
   **Set Character**. Resolve Display discovery manually if Maya reports more
   than one candidate.
6. Confirm the captured outfit and scene frame rate, then click **Connect**.
7. Unreal validates the captured Maya character snapshot against the binding
   actor's Skeletal Mesh and returns `ready`. If no binding actor exists or
   validation fails, Maya displays the returned error and disconnects.
8. Pose, play, or scrub in Maya. Unreal updates the placed actor without
   changing its world transform.
9. Click **Disconnect** when finished. No Animation Sequence is written.

Exactly one placed binding actor is required before Maya connects. Multiple
placed binding actors are rejected because the product has one character,
target Skeletal Mesh, and Live Link subject per connection.

## Maya Sampling

### Skeleton discovery and names

- The selected node must be a joint.
- The sender walks the selected deformation hierarchy with parents before
  children.
- It sends joints only; controllers and constraint nodes are never subjects.
- Each Maya namespace is removed from the transmitted leaf bone name, for
  example `Hero:spine_01` becomes `spine_01`.
- If normalization produces duplicate bone names, Maya retains their full DAG
  paths for selection. Version 0.2.0 may connect them through the Unreal remap
  rules documented in the addendum below.
- The root transform is sampled in Maya world space. Every child transform is
  sampled relative to its transmitted parent joint. This includes evaluated
  controller, constraint, IK, `jointOrient`, and upstream rig-group effects,
  while nodes above the selected root are not transmitted as bones.

### BlendShapes

The sender discovers BlendShape deformers affecting meshes skinned to the
selected skeleton. Multiple deformers may use the same alias on different mesh
parts, matching Unreal's FBX behavior of combining same-named shapes into one
Morph Target. The sender therefore groups those plugs by alias and publishes
one Live Link curve value per unique alias. Every plug in a group must evaluate
to the same value within an absolute tolerance of `1e-6`; a conflict stops
sampling with an actionable error instead of choosing or overwriting a value.

Unreal applies a curve only when the selected Skeletal Mesh contains a Morph
Target with the same name. A missing Morph Target is reported but does not
invalidate an otherwise matching skeleton.

### Sampling and handoff

A Maya-main-thread timer samples the evaluated pose at the Maya scene rate from
1 through 60 fps, including supported fractional rates. A valid time-unit
change replaces the timer without renegotiating the connection. The sender
does not register per-joint or per-BlendShape dirty callbacks; polling remains
the deliberate mechanism until production measurements justify another one.

The sampled, serialized frame replaces any older unsent frame in a one-slot
handoff to a sender thread. The worker touches no Maya API. Dropping stale
unsent frames is intentional: live preview favors the newest pose over delayed
delivery of every intermediate pose.

Editing a key away from the currently evaluated Maya time does not transmit the
whole animation range. The result becomes visible when Maya evaluates that
frame during playback or scrubbing.

## Coordinate and Unit Conversion

Maya transforms are converted to Unreal coordinates before transmission:

- Maya scene linear units are converted to centimeters.
- Maya's coordinate basis is converted to Unreal's left-handed, Z-up basis.
- Rotation is transmitted as a normalized quaternion.
- Scale is transmitted independently.
- The root uses its world transform relative to Maya's origin.
- Children use transforms relative to their transmitted parent bones.

The receiving actor never applies the streamed root transform to the Actor or
Skeletal Mesh Component transform. It applies it as the root-bone transform
inside the Live Link pose, so the actor's manually placed Unreal world transform
remains the overall offset.

## Local Protocol

- Transport: TCP.
- Listener: Unreal.
- Client: Maya.
- Endpoint: `127.0.0.1:54321`.
- Concurrent clients: one.
- Encoding: UTF-8 JSON preceded by an unsigned 64-bit, big-endian byte length.
- Protocol version: `3`.
- Subject name: `MtoU_Character`.

Message sequence:

1. `init`: protocol version, an ordered list of bone records containing each
   normalized name, parent index, and source bind-local transform, plus the
   ordered curve names.
2. `ready` or `error`: Unreal accepts or rejects the protocol, placed binding,
   and skeleton hierarchy. An error closes the connection.
3. `frame`: transforms in accepted bone order and an ordered numeric curve
   array aligned with the curve names from `init`.

After `ready`, Maya sends only `frame` messages. Socket closure communicates
transport or structural failure; Unreal logs the actionable reason locally.

The protocol has no application-defined maximum message, bone, or curve count.
Collections and buffers are sized from the received data and remain subject
only to host memory and Unreal container representation limits. The parser
still enforces three correctness invariants:

- the transform count must equal the accepted skeleton's bone count;
- the curve-value count must equal the accepted skeleton's curve count;
- transform and curve numbers must be finite, never `NaN` or infinity.

A structurally invalid message closes the connection. A frame containing a
non-finite value, or a finite double outside the Unreal Live Link float range,
is dropped, the exact bone or curve is reported, and later valid frames remain
eligible for display.

## Unreal Skeleton Validation

During `init`, Unreal requires exactly one placed binding actor and compares
the static Maya skeleton with its Skeletal Mesh. It compares normalized bone
name to parent bone name, not array position. A link succeeds when every bone
and parent can be mapped uniquely and completely. Unreal importer-added numeric
suffixes may map duplicate Maya short names only below an already mapped
parent.

The validation response lists missing bones, extra bones, and parent mismatches.
It also reports successful bone-name mappings and both directions of Morph
difference as warnings. No pose is applied to an unusable target. For a usable
target, source component-space motion relative to Maya's saved bind pose is
mapped onto the Skeletal Mesh reference pose before Live Link publication.
Permissive skeleton subsets and fuzzy name matching remain outside scope.

## UI

### Maya

The native Maya window contains:

- character setup and optional manual Display selection;
- the captured root-joint path and current outfit;
- scene frame rate, bone count, and BlendShape count;
- **Connect** / **Disconnect**;
- connection status and warning preference;
- actionable diagnostics and duplicate-bone selection.

The diagnostic-details window contains only the active error summary, solution,
stable code, and error-specific details. It omits character and scene values
already visible in the main window, wraps long lines, and expands its text area
with the window.

No host, port, subject, multi-character list, or automatic-import controls are
shown because the product has exactly one local endpoint and subject.
Repeated `run()` calls focus the existing window.

### Unreal

Unreal uses its existing Live Link panel to show source status. The plugin adds
only the binding asset type and drag-to-level behavior. The binding details show
the Skeletal Mesh. The placed actor shows its binding and transient connection
state as read-only diagnostics.

## Lifecycle and Failure Behavior

### Maya

- **Set Character** captures the selected root, Display and outfit context, and
  one immutable character snapshot revision.
- **Connect** starts one transactional streaming session, registers its
  lifecycle callbacks and timer, and sends `init`; after `ready`, it sends the
  current pose immediately.
- **Disconnect**, window close, root deletion, scene open/new, and Maya shutdown
  all remove lifecycle callbacks, stop the timer and worker, and close the
  socket.
- A failed connection attempt leaves no callbacks, timer, or worker behind.
- Outfit changes refresh the character snapshot and disconnect the active
  session. Sampling errors or a snapshot revision change require a fresh
  character capture; transport failure leaves the captured character reusable.
- Maya never modifies the scene as part of streaming.

### Unreal

- The listener binds to loopback only and starts once in Unreal Editor.
- A second Maya client is rejected with an actionable response.
- Plugin shutdown and editor exit stop the listener, join its worker, unregister
  the source, and release sockets.
- A disconnected actor clears the last streamed frame, returns to the Skeletal
  Mesh reference pose, and displays a disconnected state. Manual reconnect
  resumes updates.
- Protocol errors do not modify the Skeletal Mesh, binding asset, level actor
  transform, or other project content.
- Port conflicts appear in the Live Link source status and Unreal log.

## Performance

There is no product-level bone-count ceiling. The acceptance character may
contain 700 or more bones. The target is a visible Maya-to-Unreal update within
100 ms on the user's production workstation while editing or scrubbing the
actual production rig. Tests report achieved update rate, serialized frame
size, and latency rather than rejecting a character because of its size.

The implementation uses a scene-rate polling timer, compact JSON, and
newest-frame-wins queuing. Per-node dirty callbacks and a binary frame format
are deliberately deferred. Either is introduced only if measurements on the
production character show polling or JSON processing is the relevant
bottleneck.

## Verification

### Maya-side tests

Standard-library tests cover protocol conformance, hierarchy and transform
conversion, character-scene capture and revision behavior, streaming-session
startup rollback and terminal races, frame-rate timer replacement, worker
shutdown, and dynamically sized messages. Maya 2022 host tests exercise real
DAG, skinning, BlendShape, callback, and timer behavior through the same
character-scene and streaming-session interfaces.

Run the pure suite under repository Python with
`python -m unittest discover -s composite/MtoULiveLink/maya/MtoULiveLink/tests -p test_mtou_livelink.py -q`,
and the host suite under Maya 2022 `mayapy` with
`mayapy -m unittest discover -s composite/MtoULiveLink/maya/MtoULiveLink/tests -p maya_host_tests.py -q`.

### Unreal Automation tests

Development-only automation tests cover:

- the Unreal-applicable canonical protocol conformance cases;
- partial TCP frames and multiple messages in one receive buffer;
- `init`/`ready`, stable error categories, and connection-closing behavior;
- dynamically sized skeleton and frame parsing;
- transform- and curve-count mismatch;
- `NaN` and infinity rejection;
- connection negotiation, including parent-scoped importer suffix mappings and
  bidirectional Morph differences;
- bind-pose invariance and reference-pose mapping across different local axes,
  parent-child motion, translation, and unit scale;
- preservation of actor world transform while root-bone motion changes;
- end-to-end loopback socket flow, second-client rejection, bind failure, and
  clean idempotent source shutdown.

### Build and repository verification

- Compile both Unreal modules against stock UE 5.7.4.
- Compile and load both modules against Topia Engine 5.7.4 with
  `tools/build_mtou_topia.ps1`; keep every writable build intermediate outside
  the engine and production project.
- Load the plugin in `unreal/ToolsLab.uproject` and inspect warnings.
- Run the plugin's Unreal Automation tests.
- Run the Maya project tests with Maya 2022 `mayapy`.
- Run root repository unit tests and `tools/validate_repository.py`.
- Preview the Maya and Unreal source packages with repository packaging tools.
- Confirm no `Binaries`, `Intermediate`, caches, archives, or other generated
  files are visible to version control.

### End-to-end acceptance

1. Import a Skeletal Mesh produced by the unchanged `Input.mel` workflow.
2. Create a binding in a user-chosen Content Browser folder.
3. Drag it into a level at a non-zero world location.
4. In a Maya animation scene using the referenced or proxy rig, select its
   deformation root and connect.
5. Confirm that bone and parent validation succeeds.
6. Modify controls, IK, constraints, current-frame keys, and matching
   BlendShapes; confirm Unreal displays evaluated results.
7. Play and scrub Maya's timeline and record latency and update rate on the
   production character, including a character with more than 700 bones.
8. Confirm the Unreal actor world transform remains unchanged while root-bone
   motion is visible relative to the placed actor.
9. Disconnect and reconnect; rerun the Maya tool; open or create a scene; and
   confirm no duplicate callback, thread, source, or socket remains.
10. Attempt a link with an intentionally wrong Skeletal Mesh and confirm that
    the pose is refused with explicit hierarchy differences.

Full functional compatibility is claimed only for stock Unreal Editor 5.7.4.
The third-party-modified Topia Engine 5.7.4 installation was excluded from the
2026-08-11 scope; the 2026-08-28 Topia record now adds successful compilation
and Athena editor loading, while end-to-end production behavior remains outside
that narrower gate.

## Version 0.2.0 Addendum

Date: 2026-08-12

Version 0.2.0 keeps the single-character, local-editor architecture and adds a
Maya-side outfit-aware workflow. After the user selects the deformation root,
Maya locates the same-character `Display_ctrl` transform and the unique enum
attribute containing Clothes entries. Only effectively visible, non-intermediate
skinned meshes contribute BlendShape curves. A Clothes enum change disconnects
the active session, refreshes Maya diagnostics, and requires the user to replace
the single Unreal Binding Actor and reconnect.

Maya presents a Chinese UI with a red/green connection indicator, current
outfit, exact scene frame rate, transmitted bone count, and unique BlendShape
count. The sender samples at the actual Maya scene rate from 1 through 60 fps;
the production files supplied for acceptance use Maya `ntsc`, or 30 fps.
Changing to another valid rate rebuilds the timer without a new handshake.

The protocol version is 2. Error replies now contain stable `code`, `message`,
and `details` fields. Ready replies report both Maya curves missing in Unreal
and Unreal Morph Targets missing in Maya. Curve differences remain warnings,
while skeleton differences remain blocking. Unreal now requires exactly one
placed MtoU Binding Actor for a connection.

The then-current `SK_C04_Last09.0013.ma` production scene confirmed automatic
discovery of `|Group|MotionSystem|Add_Ctrl_grp|Display_ctrl_grp|Display_ctrl`,
its `Mod` enum, current `Clothes09`, 30 fps, 860 transmitted joints, and 127
unique visible-mesh BlendShape curves. The supplied multi-outfit
`SK_C01_SumTotal.0004.ma` confirmed the 13 Clothes enum entries, but its
deformation hierarchy currently contains three short bone names duplicated
across two different Maya DAG branches. Unreal imports the second branch with
numeric suffixes, so version 0.2.0 attempts the unique parent-scoped remap
described below and warns when it succeeds.

Duplicate-name diagnostics retain every conflicting Maya DAG path, and the Maya
error dialog and main window can select all affected joints directly. Duplicate
short names may connect when Unreal finds exactly one importer-renamed numeric
suffix candidate below the already matched parent. Live Link publishes the
actual Unreal bone name while preserving Maya transform order. The connection
reports every remap as a warning; zero or multiple candidates remain blocking.

## Protocol v4 Contract and Conformance Corpus

Protocol v4 freezes the following wire fields. Every listed field is required;
adapters ignore unknown fields so additive transport metadata remains
forward-compatible. A structural or semantic field change requires a new
protocol version.

| Message | Required fields | Contract role |
| --- | --- | --- |
| `init` | `type`, `version`, `workflow`, `blendshapes_enabled`, parent-first `bones` with bind-local transforms, `curves` | Maya character description and selected streaming mode |
| `frame` | `type`, `transforms`, `curves` | One ordered evaluated pose carrying the full curve manifest |
| `ready` | `type`, `missing_in_unreal`, `missing_in_maya`, `bone_name_remaps`, `workflow`, `target_morph_count`, `accepted_morph_count` | Successful negotiation reply |
| `error` | `type`, `code`, `message`, `details` | Stable failure reply |

When `blendshapes_enabled` is false, Maya still sends the complete `curves`
manifest, while the ready reply's `accepted_morph_count` and the published
Live Link property list both stay empty so the session remains consistent.

Model preview negotiation distinguishes an intentionally empty manifest from a
failed non-empty one. A current outfit that declares zero BlendShape names is
bone-driven by design: with BS transmission enabled it still negotiates Ready
with an empty Accepted Preview Morph set and no placeholder curve or synthetic
manifest entry, while a non-empty manifest whose intersection with the
generated library is empty keeps blocking with `PREVIEW_MORPH_MISMATCH`.
Partial and complete intersections keep their warning and Ready behavior, and
other garments' BlendShapes never satisfy the current outfit's coverage. This
clarification requires no new wire field and leaves protocol v6 framing,
message shapes, Cached Playback identity, and version negotiation unchanged.

Framing, invalid UTF-8, invalid `init`, and structurally invalid `frame`
messages use `INVALID_MESSAGE` and close the connection. An unsupported numeric
protocol version uses `PROTOCOL_VERSION_MISMATCH` and closes the connection. A
non-finite or Unreal-float-range value in an otherwise structural frame drops
only that frame and keeps the connection. An unusable connection-negotiation
outcome sends its negotiation error and then closes.

The v4 wire contract above is retained as history; its standalone corpus was
superseded and is no longer retained. The frozen v5 corpus remains beside the
current machine-authoritative `conformance-v6.json`. Maya repository tests read
v6 directly, and a Python-standard-library generator projects the same cases
into a checked-in, test-only Unreal `.inl`; repository validation fails if that
projection is stale. Neither shipped host component has a runtime dependency
on the corpus or on its sibling host directory. Protocol v5 later extends the
v4 contract with the Cached Playback transfer and control messages documented
below.

## Reference-Pose Mapping Revision

Date: 2026-08-19

Version 0.3.0 separates the Maya animation frame, Maya bind pose, and Unreal
reference pose. Maya protocol v3 bone records carry a bind-local transform
derived from the inverse of each SkinCluster `bindPreMatrix`. Bind-pose
`dagPose.worldMatrix` data supplies hierarchy members that are not direct skin
influences; the joint `bindPose` attribute connects downstream to that dagPose,
which holds the first bind pose — the pose Go to Bind Pose restores and
typically the Skeletal Mesh export pose. When skin clusters disagree because
outfits were bound at different poses, the joint-connected dagPose wins,
otherwise the skin cluster with the most influences does, and the resolved
conflict count is reported after every character capture. Equally ranked
candidates must contain equivalent matrices; a disagreement rejects capture
instead of allowing skin-cluster naming to choose the bind pose. Joints with no
stored bind data, such as corrective slider joints added after binding, use
their setup-time local offset anchored to the parent's bind frame. Raw matrices
are checked before inversion, and a non-finite or non-invertible matrix rejects
character capture instead of falling back to the current frame.

Unreal aligns the target reference pose to Maya's negotiated bone order. For
each bone, it builds source bind, source current, and target reference component
matrices. With Unreal's row-vector composition, the target current component
matrix is:

`TargetRefComponent * inverse(SourceBindComponent) * SourceCurrentComponent`

The mapped component pose is converted back to parent-local transforms for
Live Link. Therefore `SourceCurrent == SourceBind` produces exactly the target
reference pose, while arbitrary current animation remains independent of frame
1 and of the pose visible when the user connects.

## Cached Playback Revision

Date: 2026-08-21

Cached Playback is implemented entirely in the Maya component behind two
focused internal seams: `_PlaybackCache` owns incrementally written temporary
files, metadata, atomic completion, ordered iteration, replacement, stale
removal, and idempotent deletion; `_CachedPlayback` owns connection
readiness, character-snapshot revision checks, Playback Range capture,
timeline restoration, progress/cancellation, and ordered replay. The existing
`_StreamingSession` pauses its timer and callbacks without renegotiating the
character snapshot, then switches its sender to an ordered queue. The live
newest-frame slot remains unchanged for Real-time Preview.

The cache is a JSON-lines frame file plus JSON metadata in the current user's
system temporary directory. A partial cache is never replayable; metadata is
published only after the frame file is atomically finalized. Replay uses the
captured scene rate, sends the first frame immediately, never skips late
frames, and retains a completed cache across replay stop, completion, and
compatible reconnects. No Unreal-side cache player, persistent Unreal asset,
or protocol-version change was added.

Issue #7 hardens the lifecycle around that seam. Cached mode reports that
live sampling is paused and that Unreal holds the recent live pose until
replay starts; active replay explicitly reports that Unreal is showing
captured data. The UI exposes an explicit replay stop and reopens the
sender's ordered mode before every new replay after a stop. Transport failure
is distinct from an intentional disconnect: an active replay stops with its
diagnostic while a completed cache remains available for a later compatible
connection; incompatible character or scene failures and normal disconnect,
close, and exit paths delete owned files. Cached-session callbacks carry their
originating session identity, so a late terminal event cannot change a newer
controller state.

Deterministic Maya-side tests cover inclusive capture and exact ordering,
atomic completion, timeline restoration, cancellation, stale cleanup, ready
connection and revision guards, ordered replay timing, and retention across
mode switches. Issue #6 additionally validates cancellation after the final
progress callback, timer and disk-usage failure cleanup, recapture while
replaying, exact range completion, and exact MtoU-owned metadata/frame paths.
Issue #7 adds coverage for terminal transport failures, ordered-mode restart,
manual disconnect cleanup, explicit cached-state messaging, and stale session
events. The external C01 production fixture remains required for the
stock-engine 321-frame production gate; automated pure/host checks do not
claim that fixture has passed.

## Protocol v5 Upload-then-Play Cached Playback Revision

Date: 2026-08-25

Specification #16 supersedes the transport-paced cached replay above. Maya's
`_PlaybackCache` keeps owning complete capture, metadata, atomic completion,
iteration, replacement, and deletion; `_CachedPlayback` no longer paces
replay with Maya timers. After capture completes it uploads the whole cache
without a real-time deadline — `cache_begin` declares revision, captured range,
scene rate, frame count, and encoded size (bounded per-frame size times frame
count), indexed `cache_frame` messages stream in chunks through the sender's
ordered queue so Maya memory stays bounded, and `cache_end` finishes the
upload. Maya then blocks on `cache_ready`; during local replay it only drains
Unreal's control replies on a lightweight poller timer and sends no animation
data.

The Unreal runtime module gains `FMtoUCacheSession`, a game-thread transient
cache owner scoped to one negotiated session id. The worker thread parses cache
message shapes and forwards commands with their session identity; the game
thread validates semantics against negotiated bone/curve counts and frozen
bounds (64 MiB declared payload, 20 000 frames, 1–60 fps), buffers validated
frames, and transitions Idle → Receiving → Ready atomically at `cache_end`.
Partial uploads are dropped entirely and can never become Ready or replayable;
cache validation errors reply with stable codes while keeping the connection
open. While the cache owns the session, ordinary live frames are dropped and
their pending slots cleared, viewport realtime override is released during
capture/upload, re-enabled for local playback, and restored on stop, clear, or
disconnect.

Local replay applies each buffered frame exactly once, in order, through the
existing retargeted Live Link publication path, scheduled by the captured scene
rate on an injectable monotonic clock. Two guards report
`CACHED_PLAYBACK_PERFORMANCE` instead of silently degrading: publishing may not
cost more wall time than the schedule it consumed (plus one interval of slack),
and total completion time may not exceed the schedule by more than max(0.5 s,
5%). Manual stop holds the last applied frame and retains the buffer;
replay-again replays it without re-upload when the revision still matches;
recapture, revision change, clear, disconnect, tool close, and exit invalidate
coherently. No package, `.uasset`, or Content Browser asset is created.

Protocol v5 freezes the cache wire contract in
`composite/MtoULiveLink/protocol/conformance-v5.json`: `cache_begin`
(`revision`, `fps`, `start_frame`, `end_frame`, `frame_count`, `payload_size`),
indexed `cache_frame`, `cache_end`, `cache_ready` (`frame_count`),
`cache_play` (`revision`), `cache_stop`, `cache_clear`, `cache_complete`
(`frame_count`), plus stable errors `CACHE_METADATA_INVALID`,
`CACHE_PAYLOAD_TOO_LARGE`, `CACHE_FRAME_INDEX_INVALID`,
`CACHE_FRAME_CONTENTS_INVALID`, `CACHE_INVALID_STATE`, `CACHE_NOT_READY`,
`CACHE_REVISION_MISMATCH`, and `CACHED_PLAYBACK_PERFORMANCE`. The corpus also
pins state violations against a seeded cache session. Protocol v4 clients are
rejected with the normal version-mismatch error, so paired installation of
matching component versions remains required.

## Protocol v6 Truthful-Completion Revision

Date: 2026-08-25

Specification #17 hardens the v5 upload-then-play architecture without
reopening it. The negotiated character snapshot revision is now established by
`init` and echoed by `ready`; cache uploads validate their declared revision
against it, so client messages cannot invent compatibility. Every upload and
play attempt carries a monotonically increasing identity (`upload_id`,
`play_id`), and every Unreal outcome (Ready, bounded progress, completion with
applied count and elapsed duration, stopped, cleared, runtime errors) echoes
the identity that owns it. Maya drops well-formed outcomes whose identity does
not match the current operation and keeps waiting for the current one.

Unreal applies at most one cached pose per source-frame position per
game-thread update: before publishing the next pose it checks the monotonic
clock against that position's valid window, and a missed window raises the
stable `CACHED_PLAYBACK_PERFORMANCE` failure before any overdue catch-up burst
can collapse poses into a single visible tick. Publication callbacks report
acceptance; applied evidence advances only on acceptance, and successful
completion additionally requires total elapsed time within the captured-rate
bound.

Resource accounting moved into production paths: encoded bytes are metered at
the framing boundary with overflow-safe accumulation against both the declared
size and the frozen 1 GiB limit, and parsed transient memory is preflighted
from negotiated transform/curve counts against a fixed 1536 MiB budget before
allocation. Violations reject the whole upload atomically while preserving the
negotiated session. Negative frame indexes return
`CACHE_FRAME_INDEX_INVALID` on the production socket path without closing.
The ordered-to-Latest sender transition is lossless: queued controls migrate
to a carry-over queue so `cache_clear` stays ahead of the first resumed live
pose, and an explicit `cache_enter` control establishes cached ownership
(held pose, live-frame isolation, released viewport realtime) before capture.

## Deep Cached Playback Module

Date: 2026-08-29

Issue #26 deepens the Maya Cached Playback boundary without changing protocol
v6 or user-visible behavior. One `_CachedPlayback` is attached permanently to
one negotiated Animation `_StreamingSession` when that session becomes Ready;
attachment validates the negotiated Character snapshot revision but does not
pause sampling or send `cache_enter` until `enter()`.

Controller forwards only `enter()`, `leave()`, `capture()`, `replay()`,
`stop_replay()`, `cancel_capture()`, `detach()`, and `discard()`. It observes a
copied, immutable `_CachedPlaybackView` containing semantic state, progress,
cache summary, a one-delivery diagnostic, and action capabilities. Controller
does not inspect a Playback cache, internal phase, reply queue, timer, protocol
identity, or Streaming session attachment identity.

`_CachedPlayback` owns capture sequencing, temporary-cache handoff, bounded
declaration and upload, identity-matched outcomes, replay control, ordered
clear-before-resume, recovery, and cleanup. Unexpected recoverable transport
loss may return an opaque `_CachedPlaybackRetention` containing only a complete
compatible local cache; intentional lifecycle exits, partial capture,
incompatible Character revisions, and corrupt cache data discard it. A future
Ready attachment may consume that retention without exposing a cache path or
deletion interface to Controller. `_PlaybackCache` remains the disk module and
`_StreamingSession` remains the transport module.

## Documentation and Packaging

The composite project includes matching English and Chinese README content
limited to an introduction, supported versions, installation, and usage.
Internal protocol, development, and test details remain in this project history
and beside the implementation where appropriate.

The Maya and Unreal components are independently packageable with the existing
repository tools. The Topia build helper installs only local Win64 editor output
under the target plugin. Generated archives and Unreal build output remain
untracked. Version updates keep both components on the same public semantic
version.
