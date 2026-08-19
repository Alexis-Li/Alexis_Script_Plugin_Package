# MtoU Live Link

MtoU Live Link connects one evaluated Maya character to one Unreal skeletal mesh for editor-time animation preview.

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
The compatibility decision between one Maya character description and one Unreal target description before animation streaming begins. Its outcome includes blocking incompatibilities, usable differences, and any bone-name mapping required by the target.
_Avoid_: Handshake validation, init handling

**Negotiation outcome**:
The complete result of connection negotiation: whether streaming may begin, the target bone names to publish, usable differences, required bone-name mappings, or blocking incompatibilities.
_Avoid_: Ready reply, validation result

**Bone-name mapping**:
The unique correspondence from a duplicated Maya short bone name to the numeric-suffixed name created for the same bone by Unreal import. It is valid only within an already matched parent branch.
_Avoid_: Bone rename, fuzzy match

**Morph difference**:
A Morph Target name present only in the Maya character description or only in the Unreal target description. It reduces expression coverage but does not make the connection unusable.
_Avoid_: Morph error, BlendShape failure

**Protocol contract**:
The versioned wire-level agreement shared by the Maya and Unreal adapters: framing, message shapes, value limits, stable error categories, and connection-closing semantics.
_Avoid_: Protocol implementation, connection negotiation

**Conformance corpus**:
The canonical collection of language-neutral protocol cases that both host adapters must satisfy.
_Avoid_: Protocol fixtures, golden files

**Conformance case**:
One input and its expected protocol outcome, including successful messages and rejected messages with their stable category and connection-closing semantics.
_Avoid_: Test vector, sample payload
