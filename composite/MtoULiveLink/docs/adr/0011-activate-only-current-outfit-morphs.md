# Activate only Morphs accepted for the current outfit

Each Model preview binding uses a single-garment Driver Skeletal Mesh, and Maya
continues to discover BlendShapes only from the effectively visible character
and selected outfit context. Protocol v4 negotiates the Accepted Preview Morph
set as the intersection of that Maya manifest and the complete generated Driver
Morph library. Only accepted names receive streamed values; every other
generated Morph remains at zero. No outfit-name or node-prefix filter is added.
Zero accepted names block BS transmission, while a partial intersection is a
visible warning with Maya-only and Unreal-only names and counts.
