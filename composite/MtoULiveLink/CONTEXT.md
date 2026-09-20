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
ordered skeleton, bind-local pose, and streamable curve names. The skeleton
contains every joint below the selected root and the intermediate transform
nodes needed to preserve their real DAG parentage; branches without joints are
excluded. Connection
negotiation uses one snapshot and does not silently adopt topology changes
during streaming.
_Avoid_: Scene state, init payload, role data

**Streaming session**:
One connection attempt that negotiates a character snapshot with an Unreal
target and, if usable, streams evaluated poses until disconnect or failure.
_Avoid_: Worker, socket connection, sender thread

**Cached Playback**:
The Animation preview workflow that captures the Playback Range into a temporary cache, transfers the complete cache to Unreal, and controls Unreal's local replay until the cache is cleared and Real-time Preview resumes.
_Avoid_: Cache replay, offline playback, sender-paced replay

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
The formally bound Skeletal Mesh that supplies its reference skeleton, source
skin weights, and Morph Target library. It is either the production
full-character mesh containing body, face, hair, and exactly one current
outfit, or a garment-only mesh; it never aggregates several outfit variants.
Every Driver retains the same complete deformation hierarchy. Within a
character composition it is the Primary Driver: the single skeleton baseline
and the only source of garment Preview data.
_Avoid_: Source mesh, final mesh, binding mesh

**Character composition**:
The one character a Binding describes: its Primary Driver together with the
enabled Additional Parts. The composition owns which meshes pose and display
together under one streaming session and one Live Link subject; it never
introduces a second character, a second subject, or per-part connections.
_Avoid_: Multi-character setup, mesh list, attachment list

**Additional Part**:
One Skeletal Mesh of the character composition beside the Primary Driver, with
a display name, a Skeletal Mesh, and an enabled state. An enabled part shares
the Primary Driver's Skeleton asset, maps every bone it contains onto the
Primary by name and parent path, and matches the Primary's reference pose for
the bones in its skinning palettes across all LODs and their ancestors; it may
use fewer bones and different geometry, and a bone the
Primary does not have is a blocking incompatibility. A disabled part takes part
in neither negotiation nor display, and parts never contribute to garment
resolution, weight transfer, or Preview Morph transfer.
_Avoid_: Sub-mesh, attachment, extra mesh, garment part

**Driver garment surface**:
The transient set of Driver LOD0 triangles selected as corresponding to the
selected Preview Static Mesh for one Preview refresh. Automatic resolution
picks it from geometry connectivity and spatial agreement with imported
material-slot identity as supporting evidence only; the advanced manual Driver
Garment Slot Override pins it to named imported slots instead, and every
manual selection still passes the same geometry coverage and alignment
validation. It may span several sections and disconnected pieces, retains
original source-vertex correspondence, and is never a persistent asset.
_Avoid_: Garment section, source region index, resolved LOD

**Driver Garment Slot Override**:
The Binding's optional advanced list of stable imported Driver material-slot
names that manually identifies the garment source when automatic resolution is
ambiguous. An empty list keeps automatic resolution; a missing, duplicated, or
no-longer-unique name, or a Driver triangle that maps to no final material
slot, is a hard preflight error that never silently returns to Auto.
_Avoid_: Slot index override, section selection, source mesh picker

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
matching BlendShapes on the formal Driver Skeletal Mesh together with every
enabled Additional Part. Its target Morph library is the deduplicated union of
the Primary Driver and the enabled parts, so a name only some meshes own still
streams and simply drives the meshes that have it.
_Avoid_: Animation mode, Driver mode, legacy mode

**Model preview workflow**:
The modeler-facing workflow that uses the selected Maya outfit as deformation
context and displays its modified surface through a Generated Preview Skeletal
Mesh under the current pose. With a full-character Driver, the original Driver
also remains visible behind it with the resolved garment material slots hidden,
preserving body, face, hair, and other non-garment parts without extra inputs;
enabled Additional Parts stay displayed beside them. Matching live curves drive
Morph Targets on the Generated Preview, the hidden Driver, and every part that
owns the name.
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
Morph Targets the Model preview displays: the Generated Preview Skeletal Mesh,
the Primary Driver, and the enabled Additional Parts. Only this set receives
streamed values, and each name drives the meshes that own it. An outfit that
declares no BlendShape names has a valid empty accepted set; only a non-empty
manifest with an empty intersection is a failed pairing.
_Avoid_: Full Morph stream, active Morph library, transferred BS list

**Bone-only comparison**:
An explicitly requested Model preview connection with BS transmission disabled.
It may help isolate skinning behavior but is not valid evidence for model
acceptance. It differs from an intentionally bone-driven outfit whose manifest
declares zero BlendShape names while BS transmission stays enabled; that
session negotiates Ready.
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
