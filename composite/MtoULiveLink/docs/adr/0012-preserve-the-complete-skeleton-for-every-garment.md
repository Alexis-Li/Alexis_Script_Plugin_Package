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
