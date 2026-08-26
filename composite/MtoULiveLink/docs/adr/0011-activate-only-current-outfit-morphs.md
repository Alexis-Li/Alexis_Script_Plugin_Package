# Activate only Morphs accepted for the current outfit

Each Model preview binding uses a single-garment Driver Skeletal Mesh, and Maya
continues to discover BlendShapes only from the effectively visible character
and selected outfit context. Protocol v4 negotiates the Accepted Preview Morph
set as the intersection of that Maya manifest and the complete generated Driver
Morph library. Only accepted names receive streamed values; every other
generated Morph remains at zero. No outfit-name or node-prefix filter is added.
A partial intersection is a visible warning with Maya-only and Unreal-only
names and counts.

An intentionally empty manifest is not a failed pairing: when the current
outfit declares zero BlendShape names, the outfit is bone-driven by design, so
the session is Ready with an empty Accepted Preview Morph set even with BS
transmission enabled, and no placeholder curve or synthetic manifest entry is
invented for it. Only a non-empty manifest whose intersection with the
generated library is empty blocks BS transmission with `PREVIEW_MORPH_MISMATCH`.
Disabling BS transmission on an outfit that does declare BlendShapes remains
the explicitly labelled bone-only diagnostic that is excluded from model
acceptance.
