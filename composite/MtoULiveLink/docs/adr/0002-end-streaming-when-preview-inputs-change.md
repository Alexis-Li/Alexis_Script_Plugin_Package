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
