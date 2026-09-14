# Preserve the complete skeleton for every garment

Every Driver Skeletal Mesh used by Model preview retains the same full
deformation hierarchy sent by the existing Maya character capture, including
auxiliary bones not weighted by the currently selected outfit. The complete
hierarchy comprises every joint under the selected Maya root plus the
intermediate `transform` nodes that connect joints (for example `joints_grp`
between `spine_02` and the head branch); branches without joints are not part
of the published skeleton. MtoU_LiveLink
does not derive an outfit-specific bone subset or relax its exact hierarchy
negotiation, because that would change Live Link transform order and the
implemented Animation preview contract. Changing the Maya Clothes selection
disconnects; the user selects the corresponding single-outfit Driver (the
full-character mesh or a garment-only mesh) and Preview assets in the same
binding, refreshes the dirty actor, and reconnects. The Generated Preview
Skeletal Mesh always carries this complete reference skeleton even though only
the resolved garment surface participates in weight and Morph transfer.

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
