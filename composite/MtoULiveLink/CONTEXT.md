# MtoU Live Link

MtoU Live Link connects one evaluated Maya character to one Unreal target for
editor-time Animation preview or Model preview.

## Language

**Character scene**:
The currently captured Maya character: one deformation root together with the
Display and outfit context that determines which evaluated meshes belong to it.
_Avoid_: Role, Maya selection, character cache

**Character snapshot**:
A stable description of one character scene revision, including its outfit,
ordered skeleton, bind-local pose, and streamable curve names. Connection
negotiation uses one snapshot and does not silently adopt topology changes
during streaming.
_Avoid_: Scene state, init payload, role data

**Streaming session**:
One connection attempt that negotiates a character snapshot with an Unreal
target and, if usable, streams evaluated poses until disconnect or failure.
_Avoid_: Worker, socket connection, sender thread

**Connection negotiation**:
The compatibility decision between one Maya character description and one Unreal target description before pose streaming begins. Its outcome includes blocking incompatibilities, usable differences, and any bone-name mapping required by the target.
_Avoid_: Handshake validation, init handling

**Negotiation outcome**:
The complete result of connection negotiation: whether streaming may begin, the target bone names to publish, usable differences, required bone-name mappings, or blocking incompatibilities.
_Avoid_: Ready reply, validation result

**Bone-name mapping**:
The unique correspondence from a duplicated Maya short bone name to the numeric-suffixed name created for the same bone by Unreal import. It is valid only within an already matched parent branch.
_Avoid_: Bone rename, fuzzy match

**Morph difference**:
A Morph Target name present only in the Maya character description or only in
the Unreal target description. It reduces expression coverage; the selected
workflow determines whether that loss is usable.
_Avoid_: Morph error, BlendShape failure

**Pose preview**:
A preview-quality view of garment contour, thickness, intersections, and basic
stretch under the current Maya pose. It may include Preview Morph transfer but
is not evidence of final deformation quality and excludes cloth and physics.
_Avoid_: Final preview, binding preview, deformation approval

**Driver Skeletal Mesh**:
The formally bound version of exactly one garment that supplies its reference
skeleton, source skin weights, and Morph Target library. Every garment Driver
retains the same complete deformation hierarchy and represents the same garment
as its Preview Static Mesh without aggregating other outfits.
_Avoid_: Source mesh, final mesh, binding mesh

**Preview Static Mesh**:
The current modeling iteration of the same garment, aligned to its Driver
Skeletal Mesh in reference pose, origin, units, and asset-local import space. It
supplies the surface and material appearance shown in Pose preview.
_Avoid_: Target mesh, WIP mesh, preview source

**Generated Preview Skeletal Mesh**:
A non-persistent skeletal representation used only for Pose preview. It combines
the Preview Static Mesh surface with the Driver Skeletal Mesh deformation data
and is never a formal production asset.
_Avoid_: Preview asset, temporary asset, generated asset

**Animation preview workflow**:
The animator-facing workflow that displays evaluated Maya animation and
matching BlendShapes on the formal Driver Skeletal Mesh.
_Avoid_: Animation mode, Driver mode, legacy mode

**Model preview workflow**:
The modeler-facing workflow that uses the selected Maya outfit as deformation
context and displays its modified surface through a Generated Preview Skeletal
Mesh under the current pose.
_Avoid_: Model mode, Preview mode, static mode

**Preview revision**:
One coherent pairing of a Driver Skeletal Mesh and one imported state of its
Preview Static Mesh. A relevant change to either garment representation creates
a different revision.
_Avoid_: Cache version, dirty state, preview snapshot

**Preview quality warning**:
A non-blocking indication that a Generated Preview Skeletal Mesh was produced
but its transfer risk falls outside the range calibrated with the project's
accepted garment examples; it does not identify vertices proven incorrect.
_Avoid_: Transfer error, invalid preview, skin failure

**Preview readiness**:
The condition in which the current Preview revision has a complete Generated
Preview Skeletal Mesh suitable for Model preview. A Preview quality warning
does not remove readiness.
_Avoid_: Cache validity, connected state, build success

**Preview refresh**:
The explicit request to generate the current Preview revision and establish its
Preview readiness.
_Avoid_: Auto rebuild, reconnect refresh, preview update

**Preview Morph transfer**:
A preview-only projection of the Driver Skeletal Mesh Morph Targets whose local
surfaces exist on the different topology of the Preview Static Mesh during
Preview refresh. A Driver Morph with no corresponding Preview surface is
omitted with a quality warning. Maya streams only the selected outfit's
BlendShape values to drive the generated Morph Targets; it does not transmit
garment geometry or Morph deltas.
_Avoid_: BS streaming, runtime wrap, Maya mesh transfer

**BS transmission**:
The Model preview choice to include the selected Maya outfit's BlendShape names
and values in a streaming session. It drives an existing Preview Morph library
and is distinct from Preview Morph transfer.
_Avoid_: BS build, Morph transfer button, live Morph generation

**Accepted Preview Morph set**:
The name intersection between the selected Maya outfit's BlendShapes and the
Generated Preview Skeletal Mesh Morph Targets negotiated for one Model preview
streaming session. Only this set receives streamed values.
_Avoid_: Full Morph stream, active Morph library, transferred BS list

**Bone-only comparison**:
An explicitly requested Model preview connection with BS transmission disabled.
It may help isolate skinning behavior but is not valid evidence for model
acceptance.
_Avoid_: Model preview acceptance, Morph fallback, degraded preview

**Protocol contract**:
The versioned wire-level agreement shared by the Maya and Unreal adapters: framing, message shapes, value limits, stable error categories, and connection-closing semantics.
_Avoid_: Protocol implementation, connection negotiation

**Conformance corpus**:
The canonical collection of language-neutral protocol cases that both host adapters must satisfy.
_Avoid_: Protocol fixtures, golden files

**Conformance case**:
One input and its expected protocol outcome, including successful messages and rejected messages with their stable category and connection-closing semantics.
_Avoid_: Test vector, sample payload
