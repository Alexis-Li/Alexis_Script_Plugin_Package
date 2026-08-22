# Add Model preview to the existing MtoU_LiveLink product

MtoU_LiveLink 0.4.0 adds a modeler-facing Model preview workflow beside the
implemented animator-facing Animation preview workflow instead of creating a
second plugin. The Unreal Binding gains one optional Preview Static Mesh field
below its existing Skeletal Mesh field, and the Maya tool gains top-level `动画`
and `模型` workflow controls. Animation preview remains the existing behavior;
Model preview uses the same cross-host product and connection while preparing
a Generated Preview Skeletal Mesh from both Unreal inputs.
