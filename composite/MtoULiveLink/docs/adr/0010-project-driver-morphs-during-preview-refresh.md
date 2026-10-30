# Project Driver Morph Targets during preview refresh

Model preview implements `传递 BS` by projecting the Driver Skeletal Mesh Morph
Target library onto Preview Static Mesh topology during explicit Preview
refresh. The builder computes a reusable Driver-surface triangle and barycentric
mapping, applies Driver Morph deltas to produce Preview-topology morph meshes,
and writes them to the transient Generated Preview Skeletal Mesh through stock
Unreal 5.7.4 public APIs. Maya then streams only BlendShape names and values.
MtoU_LiveLink does not stream garment vertices each frame or add a runtime
surface-wrap/deformer path for V1.

Preview Morph transfer is part of the Model preview acceptance contract.
Refresh checks
the complete Driver Morph Target library and projects every Morph whose affected
surface exists on the Preview because it runs before Maya supplies its selected
outfit curve names.

Refresh is all-or-nothing across non-empty Driver Morph projections: one failed
projection or generated Morph build puts the Preview status in `Error`, discards
the partial Generated Preview Skeletal Mesh, and restores the bound Driver for
inspection without making Model preview ready. Connection-time acceptance,
including intentionally empty manifests and bone-only diagnostics, is defined
solely by [ADR 0011](0011-activate-only-current-outfit-morphs.md).

A Driver Morph whose affected local surface is absent from the Preview Static
Mesh is not a failed projection: no Preview vertex can carry that deformation.
Refresh omits that name from the Generated Preview Morph library, completes with
a quality warning. Omission from the generated mesh does not remove a name
owned by the Primary Driver or an enabled Additional Part from the negotiated
display library. Malformed source Morphs and failures while writing
any non-empty projected Morph remain transactional errors.
