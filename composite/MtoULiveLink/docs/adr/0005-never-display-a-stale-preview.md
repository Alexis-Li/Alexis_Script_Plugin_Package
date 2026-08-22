# Never display a stale preview

When a Preview revision becomes dirty or fails to generate, its binding actor
releases the previous Generated Preview Skeletal Mesh and hides the display
component. It neither keeps showing an obsolete garment nor falls back to the
Driver Skeletal Mesh, because either result could be mistaken for the modeler's
current iteration. Display resumes only after an explicit Preview refresh
successfully produces the current revision.
