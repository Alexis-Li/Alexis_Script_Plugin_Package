# Keep generated preview state on the binding actor

The MtoU_LiveLink Binding persists only the Driver Skeletal Mesh and optional
Preview Static Mesh, while each placed binding actor owns its Generated Preview
Skeletal Mesh, status, diagnostics, and refresh action. This keeps disposable
editor-session state out of project assets and gives it the same world and
garbage-collection lifetime as the component that displays it. A disconnected
actor retains an unchanged generated mesh in reference pose for reuse, but a
new Preview revision, actor destruction, world unload, or editor shutdown ends
that cache lifetime.
