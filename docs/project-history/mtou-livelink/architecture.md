# MtoU_LiveLink Architecture

Date: 2026-07-31
Status: Version 0.3.0 implemented and locally verified; unreleased
Production acceptance baseline: Stock Unreal Editor 5.7.4 completed 2026-08-11
Last aligned with implementation: 2026-08-19

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
- Unreal Editor only.

The stock UE 5.7.4 installation is the baseline compilation target. The user's
production project uses a third-party-modified UE 5.7 build. Compatibility with
that build must not be claimed until the plugin is also compiled and exercised
with that engine installation.

## Names and Repository Locations

The user-facing product and Unreal `FriendlyName` are `MtoU_LiveLink`.
Technical identifiers omit the underscore to satisfy repository and Unreal
module conventions:

- Maya tool: `composite/MtoULiveLink/maya/MtoULiveLink/`
- Unreal plugin: `composite/MtoULiveLink/unreal/MtoULiveLink/`
- Runtime module: `MtoULiveLink`
- Editor module: `MtoULiveLinkEditor`

Both components share product version `0.3.0`. The composite project root owns
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

Run the pure suite under repository Python and the host suite under Maya 2022
`mayapy`.

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

Compatibility is claimed only for stock Unreal Editor 5.7.4. The user's
third-party-modified UE 5.7 installation was explicitly excluded from the
2026-08-11 acceptance scope; it must pass the same checks before compatibility
with that engine is reported.

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

The supplied `SK_C04_Last09.0013.ma` production scene confirmed automatic
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

## Protocol v3 Contract and Conformance Corpus

Protocol v3 freezes the following wire fields. Every listed field is required;
adapters ignore unknown fields so additive transport metadata remains
forward-compatible. A structural or semantic field change requires a new
protocol version.

| Message | Required fields | Contract role |
| --- | --- | --- |
| `init` | `type`, `version`, parent-first `bones` with bind-local transforms, `curves` | Maya character description |
| `frame` | `type`, `transforms`, `curves` | One ordered evaluated pose |
| `ready` | `type`, `missing_in_unreal`, `missing_in_maya`, `bone_name_remaps` | Successful negotiation reply |
| `error` | `type`, `code`, `message`, `details` | Stable failure reply |

Framing, invalid UTF-8, invalid `init`, and structurally invalid `frame`
messages use `INVALID_MESSAGE` and close the connection. An unsupported numeric
protocol version uses `PROTOCOL_VERSION_MISMATCH` and closes the connection. A
non-finite or Unreal-float-range value in an otherwise structural frame drops
only that frame and keeps the connection. An unusable connection-negotiation
outcome sends its negotiation error and then closes.

The machine-authoritative conformance corpus is
`composite/MtoULiveLink/protocol/conformance-v3.json`. Maya repository tests
read it directly. A Python-standard-library generator projects the same cases
into a checked-in, test-only Unreal `.inl`; repository validation fails if that
projection is stale. Neither shipped host component has a runtime dependency
on the corpus or on its sibling host directory.

## Reference-Pose Mapping Revision

Date: 2026-08-19

Version 0.3.0 separates the Maya animation frame, Maya bind pose, and Unreal
reference pose. Maya protocol v3 bone records carry a bind-local transform
derived from the inverse of each SkinCluster `bindPreMatrix`. Bind-pose
`dagPose.worldMatrix` data supplies hierarchy members that are not direct skin
influences. Every available matrix for a joint must agree; a missing or
inconsistent matrix rejects character capture instead of falling back to the
current frame.

Unreal aligns the target reference pose to Maya's negotiated bone order. For
each bone, it builds source bind, source current, and target reference component
matrices. With Unreal's row-vector composition, the target current component
matrix is:

`TargetRefComponent * inverse(SourceBindComponent) * SourceCurrentComponent`

The mapped component pose is converted back to parent-local transforms for
Live Link. Therefore `SourceCurrent == SourceBind` produces exactly the target
reference pose, while arbitrary current animation remains independent of frame
1 and of the pose visible when the user connects.

## Documentation and Packaging

The composite project includes matching English and Chinese README content
limited to an introduction, supported versions, installation, and usage.
Internal protocol, development, and test details remain in this project history
and beside the implementation where appropriate.

The Maya and Unreal components are independently packageable with the existing
repository tools. Generated archives and Unreal build output remain untracked.
Version updates keep both components on the same public semantic version.
