# End streaming when preview inputs change

A Preview mode streaming session ends when its Driver Skeletal Mesh, Preview
Static Mesh, or their relevant imported data changes. The negotiated skeleton,
reference pose, transferred weights, and displayed surface describe one coherent
input revision; hot-swapping part of that revision would make the visible result
ambiguous. MtoU_LiveLink therefore marks the preview dirty and requires a
successful refresh and new connection instead of silently falling back to the
Driver Skeletal Mesh or changing targets inside an accepted session.
