# Preserve the complete skeleton for every garment

Revised by Issue #55: Maya capture still freezes every joint below the selected
root and the intermediate transforms connecting joints. Branches without joints
remain excluded. Unreal now negotiates the union of enabled meshes' positive
skin influences across every LOD and their complete ancestors; the Primary need
not contain other parts' secondary branches. Non-required exported branches do
not block connection. Each required bone must map uniquely under an already
mapped parent using the existing exact/numeric/hash rules. All exact-name
sources under a mapped parent reserve their targets before considering import
renames. Two Maya siblings with the same published short name are ambiguous
even if a free suffixed target could absorb one; capture order cannot decide
which animation drives either bone. A rename candidate cannot take a target
owned by an exact-name source, and equal claims reject the connection with the
target, its parent, and the competing Maya paths.

The wire snapshot and cached frames remain complete (protocol v6 unchanged).
Unreal freezes a source-index projection, target reference pose and target
hierarchy for the session. Both live and cached frames use that projection
before component-space bind/current conversion and publication to one subject.
Each part evaluates that subject against its own mesh. The Generated Preview
retains the Primary reference skeleton and uses only Primary garment data; the
original Driver can therefore continue following that Generated Preview.
Composition edits and reimports terminate the session before mappings or cached
frames can be reused. Changing the Maya Clothes selection still disconnects.

Complete skeletons can include auxiliary joints with tiny non-zero scales
(such as pupil joints at `1e-12`). These nodes remain in the hierarchy. Pose
validation uses checked finite matrix inversion, not an absolute determinant
threshold; output decomposition preserves tiny axes instead of clamping them to
zero. Truly singular or numerically non-finite transforms still stop streaming.
The Maya host regression reconstructs published bind and animated TRS through
multiple structural parents independently against Maya world matrices in
centimeters (`1e-6` element-wise tolerance), reversing the wire X/Z/Y basis.
This verifies TRS chains, not arbitrary local shear or a full character's
interactive preview acceptance.

The actual character's rendered Animation/Model, Refresh and reconnect results
are recorded in the [complete-skeleton acceptance record](../../../../docs/project-history/mtou-livelink/complete-skeleton-acceptance.md).
