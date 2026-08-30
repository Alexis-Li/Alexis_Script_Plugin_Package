# Add Model preview to the existing MtoU_LiveLink product

MtoU_LiveLink 0.4.0 adds a modeler-facing Model preview workflow beside the
implemented animator-facing Animation preview workflow instead of creating a
second plugin. The Unreal Binding gains one optional Preview Static Mesh field
below its existing Skeletal Mesh field, and the Maya tool gains top-level `动画`
and `模型` workflow controls. Animation preview remains the existing behavior;
Model preview uses the same cross-host product and connection while preparing
a Generated Preview Skeletal Mesh from both Unreal inputs. For a full-character
Driver, the actor displays that generated garment over a second display of the
same Driver with the resolved original garment material slots hidden. The
second display follows the Generated Preview's complete skeleton through Leader
Pose. Imported polygon-group identities map resolved geometry to final material
slots, and Refresh rejects a slot shared by garment and visible non-garment
groups. Model negotiation uses the union of both display meshes' Morph Targets;
accepted curves are also applied to matching Driver Morph Targets because Unreal
5.7 follower propagation is unreliable without material curves. Model preview
therefore preserves body, face, hair, and other non-garment parts and Morphs
without adding Binding inputs. Garment-only Drivers retain the same result
because every visible Driver slot is replaced.
