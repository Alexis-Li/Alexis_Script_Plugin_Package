# Preserve the complete skeleton for every garment

Every garment Driver Skeletal Mesh used by Model preview retains the same full
deformation hierarchy sent by the existing Maya character capture, including
auxiliary bones not weighted by the currently selected outfit. MtoU_LiveLink
does not derive an outfit-specific bone subset or relax its exact hierarchy
negotiation, because that would change Live Link transform order and the
implemented Animation preview contract. Changing the Maya Clothes selection
disconnects; the user selects the corresponding single-garment Driver and
Preview assets in the same binding, refreshes the dirty actor, and reconnects.
