# Keep generated preview meshes transient

Normal Pose preview must not create a Content Browser asset or `.uasset` because
generated meshes are disposable views of modeling iterations, not production
dependencies. MtoU_LiveLink will support an explicit debug-save action, but it
will not fall back to hidden temporary assets; if stock Unreal Editor 5.7.4
cannot build, render, retain, and safely discard an in-memory Generated Preview
Skeletal Mesh, Preview Mesh V1 will pause rather than weaken this boundary.
