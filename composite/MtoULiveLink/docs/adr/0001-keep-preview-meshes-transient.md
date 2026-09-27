# Keep generated previews transient, actor-owned, and explicitly refreshed

Normal Pose preview must not create a Content Browser asset or `.uasset` because
generated meshes are disposable views of modeling iterations, not production
dependencies. MtoU_LiveLink provides no save or export path for Generated
Preview Skeletal Meshes and does not fall back to hidden temporary assets. A
host that cannot build, render, retain, and safely discard the mesh in memory is
unsupported rather than weakening this boundary.

The Binding asset persists Preview inputs and settings; each placed Binding
Actor owns readiness, diagnostics, the refresh action, and the generated mesh.
Disposable state therefore shares the displaying actor's world and lifetime.
Only an explicit **Refresh Preview** request generates the current revision.
Placement, level loading, reimport, and connection may invalidate or report
readiness but never start generation implicitly: synchronous editor mesh
generation may outlast connection negotiation.

A dirty revision releases the previous generated mesh and hides the preview.
A failed refresh discards stale or partial results, keeps Error readiness, and
restores the Driver for inspection; displaying the Driver does not establish
Model readiness. A disconnected actor may retain an unchanged generated mesh
in reference pose for reuse. A new revision, actor destruction, world unload,
or editor shutdown ends that lifetime. Session invalidation is defined in
[ADR 0002](0002-end-streaming-when-preview-inputs-change.md).

Garment comparison changes only which garment is displayed. Both selections
must receive the same negotiated pose and accepted Morph values, retain the
body and enabled Additional Parts, and preserve readiness and the session.
Invalidation releases the generated mesh in either selection. Display and pose
evaluation details and remaining visual acceptance are recorded in the
[comparison record](../../../../docs/project-history/mtou-livelink/issue-51-garment-comparison-acceptance.md).

This lifetime boundary applies to Generated Preview Skeletal Meshes. It does
not decide storage or transport for the separately scoped scene-reference
prototype in [Issue #53](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/53).
