# Keep generated preview meshes transient

Normal Pose preview must not create a Content Browser asset or `.uasset` because
generated meshes are disposable views of modeling iterations, not production
dependencies. MtoU_LiveLink provides no save or export path for Generated
Preview Skeletal Meshes and does not fall back to hidden temporary assets. A
host that cannot build, render, retain, and safely discard the mesh in memory is
unsupported rather than weakening this boundary.
