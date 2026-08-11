# MtoU_LiveLink Design

Date: 2026-07-31
Status: Implemented; stock-engine production acceptance completed 2026-08-11

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
- Skeleton retargeting or partial skeleton matching.
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

Both projects use version `0.1.0` initially and own matching English and Chinese
READMEs, a changelog, license, and project tests.

## Architecture

### Maya tool

The Maya side is one directly runnable Python file:

`composite/MtoULiveLink/maya/MtoULiveLink/scripts/MtoULiveLink.py`

It owns:

- the small native Maya UI;
- deformation-root selection and validation;
- joint hierarchy and BlendShape discovery;
- namespace normalization;
- Maya-to-Unreal transform conversion;
- Maya callback registration and cleanup;
- compact JSON message creation;
- a single localhost TCP client and sender worker.

The file exposes `run()` and declares the sole Maya runtime version. Pure helper
functions remain importable outside Maya so the protocol and conversion logic
can be tested without loading the host.

### Unreal plugin

The Runtime module owns:

- the localhost listener and single accepted Maya connection;
- protocol parsing and validation;
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
5. In the Maya animation scene, select the deformation skeleton root, including
   a namespaced root such as `Hero:root`.
6. Run `MtoULiveLink.py` and click **Connect**. Connect captures and validates
   the currently selected root joint.
7. Unreal validates the Maya skeleton against the placed binding actor's
   Skeletal Mesh and returns `ready`. If no binding actor exists or validation
   fails, Maya displays the returned error and disconnects.
8. Pose, play, or scrub in Maya. Unreal updates the placed actor without
   changing its world transform.
9. Click **Disconnect** when finished. No Animation Sequence is written.

A placed binding actor is required before Maya connects. Multiple instances of
the same binding may consume the one subject and keep independent world
transforms. Simultaneously placed bindings that reference different skeletal
hierarchies are outside the single-character scope and must not be reported as
successfully linked.

## Maya Sampling

### Skeleton discovery and names

- The selected node must be a joint.
- The sender walks the selected deformation hierarchy with parents before
  children.
- It sends joints only; controllers and constraint nodes are never subjects.
- Each Maya namespace is removed from the transmitted leaf bone name, for
  example `Hero:spine_01` becomes `spine_01`.
- If normalization produces duplicate bone names, connection is refused and
  the duplicates are listed.
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

A Maya-main-thread timer samples the evaluated pose 30 times per second while
connected. Version 0.1.0 does not register per-joint or per-BlendShape dirty
callbacks. The fixed polling rate is measured against the production character
before adding another change-detection mechanism.

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
- Protocol version: `1`.
- Subject name: `MtoU_Character`.

Message sequence:

1. `init`: protocol version, an ordered list of bone records containing each
   normalized name and parent index, and the ordered curve names.
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
non-finite value is dropped, the exact bone or curve is reported, and later
valid frames remain eligible for display.

## Unreal Skeleton Validation

During `init`, Unreal requires a placed binding actor and compares the static
Maya skeleton with its Skeletal Mesh. It compares normalized bone name to parent
bone name, not array position. A link succeeds only when every Maya bone exists
in the Unreal reference skeleton, every Unreal reference bone exists in the
Maya skeleton, and every parent relationship matches.

The validation response lists missing bones, extra bones, and parent mismatches.
No pose is applied to a mismatched actor. Retargeting, permissive subsets, and
automatic bone-name maps are not part of version 0.1.0.

## UI

### Maya

The native Maya window contains only:

- the connected root-joint path;
- **Connect** / **Disconnect**;
- connection status;
- the last actionable error.

No host, port, subject, multi-character list, or automatic-import controls are
shown because the first version has exactly one local endpoint and subject.
Repeated `run()` calls focus the existing window.

### Unreal

Unreal uses its existing Live Link panel to show source status. The plugin adds
only the binding asset type and drag-to-level behavior. The binding details show
the Skeletal Mesh. The placed actor shows its binding and transient connection
state as read-only diagnostics.

## Lifecycle and Failure Behavior

### Maya

- **Connect** validates the selected root before opening the socket.
- Connection registers lifecycle callbacks once and sends `init`; after
  `ready`, it sends the current pose immediately.
- **Disconnect**, window close, root deletion, scene open/new, and Maya shutdown
  all remove lifecycle callbacks, stop the timer and worker, and close the
  socket.
- A failed connection attempt leaves no callbacks or worker behind.
- Version 0.1.0 does not detect bone rename, insertion, deletion, or reparenting
  during a session. The user disconnects and reconnects after topology edits.
- Maya never modifies the scene as part of streaming.

### Unreal

- The listener binds to loopback only and starts once in Unreal Editor.
- A second Maya client is rejected with an actionable response.
- Plugin shutdown and editor exit stop the listener, join its worker, unregister
  the source, and release sockets.
- A disconnected actor keeps the last valid pose and displays a disconnected
  state. Manual reconnect resumes updates.
- Protocol errors do not modify the Skeletal Mesh, binding asset, level actor
  transform, or other project content.
- Port conflicts appear in the Live Link source status and Unreal log.

## Performance

There is no product-level bone-count ceiling. The acceptance character may
contain 700 or more bones. The target is a visible Maya-to-Unreal update within
100 ms on the user's production workstation while editing or scrubbing the
actual production rig. Tests report achieved update rate, serialized frame
size, and latency rather than rejecting a character because of its size.

The first implementation uses a fixed 30 Hz polling timer, compact JSON, and
newest-frame-wins queuing. Per-node dirty callbacks and a binary frame format
are deliberately deferred. Either is introduced only if measurements on the
production character show polling or JSON processing is the relevant
bottleneck.

## Verification

### Pure Maya-side tests

A small standard-library test module covers:

- namespace stripping;
- duplicate normalized names;
- hierarchy construction;
- length-prefix framing;
- coordinate, quaternion, scale, and unit conversion;
- finite-number validation;
- dynamically sized messages with more than 700 bones.

Run it first under repository Python, then under Maya 2022 `mayapy`.

### Unreal Automation tests

Development-only automation tests cover:

- partial TCP frames and multiple messages in one receive buffer;
- `init`/`ready` and protocol-version rejection;
- dynamically sized skeleton and frame parsing;
- transform- and curve-count mismatch;
- `NaN` and infinity rejection;
- order-independent skeleton hierarchy matching;
- detailed missing, extra, and wrong-parent diagnostics;
- preservation of actor world transform while root-bone motion changes;
- clean source and socket shutdown.

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

## Documentation and Packaging

Each side includes matching English and Chinese README content limited to an
introduction, supported versions, installation, and usage. Internal protocol,
development, and test details remain in project development documentation.

The Maya and Unreal components are independently packageable with the existing
repository tools. Generated archives and Unreal build output remain untracked.
Version updates keep both components on the same public semantic version.
