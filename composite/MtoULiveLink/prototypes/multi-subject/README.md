# Independent prop and second character preview (Issue #54)

A bounded, **two-subject realtime** Maya 2024 → Unreal 5.7 Editor experiment.
It takes one full-body character and one independently bound skeletal prop, then
replaces the second target with a different-skeleton arms rig after a fresh
negotiation. It reads evaluated animation from Maya reference instances and
applies each subject to its explicitly paired Unreal Skeletal Mesh/Actor at
one source frame. It does not modify MtoULiveLink product protocol v9 or its
single Binding Actor, build a multi-object cache, retarget skeletons, change
animation import/export, or package this prototype as a production plugin.

- [Wire, identity, space, cache-only design and product cutover inventory](protocol.md)
- [Maya fixture, sender and checks](maya/MtoUMultiSubjectPrototype/README.md)
- [Unreal receiver, explicit preview takeover, console commands and checks](unreal/MtoUMultiSubjectPrototype/README.md)

## Reproduce

1. In this checkout, build `unreal/ToolsLab.uproject` with the local UE 5.7
   `Engine/Build/BatchFiles/Build.bat UnrealEditor Win64 Development
   <checkout>/unreal/ToolsLab.uproject -WaitMutex -NoHotReloadFromIDE -NoUBA`.
   ToolsLab enables the editor-only prototype through its additional plugin
   directories. This is separate from the user's production Backups project.
2. Run the Unreal Editor automation tests `MtoUMultiSubjectPrototype`.
   The opt-in `RealMayaPeer` test requires the host executable and checked-in
   peer script; pass `-MtoUMultiSubjectMayapy=<Maya2024>/bin/mayapy.exe`,
   `-MtoUMultiSubjectPeer=<checkout>/composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype/tests/maya_peer.py`,
   and `-MtoUEvidence=<writable scratch directory>`. Add
   `-MtoUMultiSubjectScenario=character-arms` for the second run. The test
   creates transient target actors/meshes and starts a disposable Maya process
   which creates its own two referenced rig files in scratch, not in the
   production scene. The UE suite also opens a real transient Sequencer with
   an existing skeletal animation track, checks exclusive control, and exits
   to the previous animation state.
3. Inspect the peer and receiver JSON reports in the evidence directory:
   subject IDs, source times and serials, bone and Morph values, measured pose
   and anchored-root disagreement, error codes, target and writer state after
   removal, socket disconnect and world teardown. The user-supplied `.ma` and
   Backups `.uproject` are **read-only** optional inspection resources; they
   are not part of the generated miniature test scene.

To run the receiver from ToolsLab's editor console, use
`MtoUMultiSubject.Listen scenario=character-prop out=<scratch directory>`, run
the Maya peer with the printed loopback port, inspect the per-target evidence,
and finish with `MtoUMultiSubject.Stop`. The fixture lives in a separate
transient editor world and is not opened in the current viewport; the evidence
is a pose/state check, not a screenshot. The exact peer invocation and inputs
are in the Maya and Unreal READMEs. Relaunch the listener with
`scenario=character-arms` after leaving the prop preview. An attached socket
is refused when the Maya root already includes parent world motion; place the
actor as a world anchor instead.

The two rigs and the Level Sequence are synthesized, not imported production
assets. Their skeletal/morph signatures deliberately overlap *between*
subjects while their skeletons differ, so isolation and mismatched-target
refusal are observable. A match in this fixture does not imply that a real
production Skeleton, attachment chain, or Sequencer asset is compatible: run
the explicit pair negotiation before previewing it. No asset save is required
or performed by the sample.
