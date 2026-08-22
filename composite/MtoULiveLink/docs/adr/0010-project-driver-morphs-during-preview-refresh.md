# Project Driver Morph Targets during preview refresh

Model preview implements `传递 BS` by projecting the Driver Skeletal Mesh Morph
Target library onto Preview Static Mesh topology during explicit Preview
refresh. The builder computes a reusable Driver-surface triangle and barycentric
mapping, applies Driver Morph deltas to produce Preview-topology morph meshes,
and writes them to the transient Generated Preview Skeletal Mesh through stock
Unreal 5.7.4 public APIs. Maya then streams only BlendShape names and values.
MtoU_LiveLink will not stream garment vertices each frame or add a runtime
surface-wrap/deformer path for V1.

Preview Morph transfer is a Model preview 0.4.0 release gate rather than an
optional later enhancement: a bone-only result does not provide the required
pose reference. Refresh processes the complete Driver Morph Target library
because it runs before Maya supplies its selected outfit curve names. If either
the transient Skin preview or complete Preview Morph transfer Spike fails, the
Model preview workflow does not ship as a partial feature.

Refresh is all-or-nothing across the Driver Morph Target library: one failed
projection or generated Morph build puts the Preview status in `Error`, discards
the partial Generated Preview Skeletal Mesh, and leaves the actor hidden. During
connection, BS transmission with no accepted Preview Morph is blocking; a
partial name intersection is a visible quality warning. Users may explicitly
disable BS transmission for a labelled bone-only comparison, but that state is
excluded from Model preview acceptance.
