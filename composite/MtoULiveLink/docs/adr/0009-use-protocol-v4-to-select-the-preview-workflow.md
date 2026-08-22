# Use protocol v4 to select the preview workflow

MtoU_LiveLink 0.4.0 upgrades the protocol to v4 because the Maya `动画` or
`模型` workflow selection determines whether Unreal displays the Driver
Skeletal Mesh or the ready Generated Preview Skeletal Mesh from the same
binding. This is connection-negotiation semantics rather than additive
diagnostic metadata, so it must not be hidden inside protocol v3. Animation
preview preserves its implemented behavior, while Model preview rejects a
binding whose current Preview revision is not ready; Maya and Unreal 0.4.0
components are installed together.

Protocol v4 requires `workflow` (`animation` or `model`) and
`blendshapes_enabled` on `init`. A `ready` reply echoes `workflow` and reports
`target_morph_count` and `accepted_morph_count` in addition to the existing
Morph differences and bone-name mappings. Model preview uses the stable errors
`PREVIEW_NOT_READY`, `PREVIEW_BUILD_FAILED`, and `PREVIEW_MORPH_MISMATCH`.
Both workflows continue to share `127.0.0.1:54321`, the `MtoU_Character`
subject, and the one-session rule. The v4 conformance corpus covers both
workflows and rejects protocol v3 with `PROTOCOL_VERSION_MISMATCH`.
