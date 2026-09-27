# Select Animation or Model preview within one product and binding

Animation and Model preview share the MtoU_LiveLink product, Binding, and
connection machinery instead of requiring separate plugins. Animation displays
the Primary Driver and enabled Additional Parts. Model displays a generated
garment with the remaining character, preserving body, face, hair, and parts;
garment comparison may select the original garment without changing workflows.

Maya selects the workflow during versioned connection negotiation. This changes
target compatibility and display semantics, so it is a protocol contract rather
than optional diagnostic metadata. Model requires the current Preview revision
to be ready; Animation does not. Both components must come from the same release.
The active protocol version, message fields, and rejection cases are maintained
in the [protocol corpus](../../protocol/conformance-v8.json), not duplicated here.
Morph acceptance is defined by [ADR 0011](0011-activate-only-current-outfit-morphs.md).

The current product supports one streaming session and one `MtoU_Character`
subject over localhost. This is the current single-character implementation
boundary, not a permanent prohibition on multiple independent objects.
[Issue #54](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54)
validates independent pairing, subject routing, and session isolation before a
production extension revises this decision. Additional Parts remain components
of one character sharing a Skeleton asset; independent props or different
skeleton targets must not bypass pairing by being classified as parts.

Full-character display implementation and material-slot constraints are recorded
in the [architecture history](../../../../docs/project-history/mtou-livelink/architecture.md).
Generated mesh ownership and garment comparison invariants are defined by
[ADR 0001](0001-keep-preview-meshes-transient.md).
