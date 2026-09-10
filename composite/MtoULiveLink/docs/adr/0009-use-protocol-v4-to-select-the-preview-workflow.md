# Select the preview workflow during versioned connection negotiation

Protocol v4 introduced the Maya `动画` or `模型` workflow selection, which
determines whether Unreal displays the Driver
Skeletal Mesh or the ready Generated Preview Skeletal Mesh from the same
binding. This is connection-negotiation semantics rather than additive
diagnostic metadata, so it required a protocol version change. Current protocol
v6 retains this contract: Animation preview preserves its implemented behavior,
while Model preview rejects a binding whose current Preview revision is not
ready; Maya and Unreal components must be installed from the same release.

Protocol v4 requires `workflow` (`animation` or `model`) and
`blendshapes_enabled` on `init`. A `ready` reply echoes `workflow` and reports
`target_morph_count` and `accepted_morph_count` in addition to the existing
Morph differences and bone-name mappings. Model preview uses the stable errors
`PREVIEW_NOT_READY`, `PREVIEW_BUILD_FAILED`, and `PREVIEW_MORPH_MISMATCH`.
Both workflows continue to share `127.0.0.1:54321`, the `MtoU_Character`
subject, and the one-session rule. The current v6 conformance corpus covers both
workflows and rejects older protocols with `PROTOCOL_VERSION_MISMATCH`.
