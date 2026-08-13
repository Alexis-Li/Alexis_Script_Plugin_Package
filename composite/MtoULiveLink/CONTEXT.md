# MtoU Live Link

MtoU Live Link connects one evaluated Maya character to one Unreal skeletal mesh for editor-time animation preview.

## Language

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
