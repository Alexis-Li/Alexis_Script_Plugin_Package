# Keep preview readiness actor-owned, explicit, and never stale

The MtoU_LiveLink Binding persists Preview inputs and settings, while each
placed Binding Actor owns its coherent Preview readiness, diagnostics, refresh
action, and transient Generated Preview Skeletal Mesh. This keeps disposable
editor-session state out of project assets and gives it the same world and
garbage-collection lifetime as the component that displays it.

Only an explicit **Refresh Preview** request may generate the current Preview
revision. Placement, level loading, reimport, and Maya connection may invalidate
or report readiness but never start generation implicitly: generation uses
editor-only mesh APIs synchronously on Unreal's Game Thread and may outlast
connection negotiation.

A dirty or failed revision releases the previous Generated Preview Skeletal
Mesh and hides the display instead of showing obsolete geometry or falling back
to the Driver. A disconnected actor may retain an unchanged generated mesh in
reference pose for reuse, but a new revision, actor destruction, world unload,
or editor shutdown ends that lifetime.
