# End streaming when preview inputs change

An Animation or Model streaming session ends when its Driver Skeletal Mesh, Preview
Static Mesh, or their relevant imported data changes. The negotiated skeleton,
reference pose, transferred weights, and displayed surface describe one coherent
input revision; hot-swapping part of that revision would make the visible result
ambiguous. MtoU_LiveLink therefore marks the preview dirty and requires a
successful refresh and new connection instead of silently falling back to the
Driver Skeletal Mesh or changing targets inside an accepted session.

Explicit Preview refresh also ends the active session before releasing or
replacing its display, even when the inputs have not changed. The synchronous
publication gate rejects old live frames and cached commands while the worker
finishes disconnecting. Successful refresh displays the Generated Preview while
disconnected; failure keeps Error readiness and restores the Driver for
inspection. Neither outcome resumes streaming without a fresh connection.

Character composition is deliberately a separate classification from the
Preview revision. Adding, removing, enabling, disabling, or replacing an
Additional Part changes which meshes the one character contains, so it ends the
active session at the same termination boundary and requires a new connection:
frames and cached commands negotiated for the previous composition can never
reach the new display components. It is not a Preview revision change. The
Primary Driver and the imported Preview Static Mesh still own garment
resolution, weight transfer, and Preview Morph transfer, so a composition change
keeps an already generated garment Preview and its readiness; only a Primary
Driver, Preview Static Mesh, Garment Slot Override, or related imported-data
change invalidates the revision and requires an explicit refresh. Pure
presentation edits inside the composition, such as renaming a part or
reordering the list, change neither the session nor the Preview.
