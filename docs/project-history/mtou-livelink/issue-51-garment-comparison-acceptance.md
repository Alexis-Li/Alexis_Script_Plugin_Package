# Issue #51: original garment and Generated Preview comparison

Date: 2026-09-28. Accepted at `867f2994b5b22febe2afd9e712e303453c6cdeb7`
on `codex/mtou-preview-workflow`. Independent natural-tick verification and
production C01 character acceptance passed, including rendered BaseColor
comparison. No blocking defect remains for Issue #51. The user authorized
scripted acceptance and screenshots in place of unnecessary desktop automation.

## Current contract

The Unreal Binding Actor Details control alternates the original Driver outfit
and the current Generated Preview without replacing its transient mesh, changing
Preview readiness, or ending the Model streaming session. The Generated mesh
remains the Live Link pose driver; the original Driver follows its bones and
receives accepted Morph values. The actor explicitly evaluates the hidden
generated mesh during original comparison so new streamed poses also reach the
visible Driver. The body's non-garment material slots and enabled Additional
Parts remain displayed. Restoring Generated hides only the garment slots
resolved by the current Preview refresh. A missing, dirty, building, or failed
Preview disables comparison; failed refresh and input changes retain their
existing inspection and invalidation behavior.

The Model negotiation already accepts the selected Maya outfit's Morph names
from the union of Generated Preview, Primary Driver, and enabled Additional
Parts. A Driver-only Morph remains accepted and active during comparison, so
no comparison-specific protocol change was needed. The feature was introduced
with components 0.8.0 and protocol v8; the final acceptance baseline includes
Issue #50 and uses components 0.9.0 and protocol v9.

## Development verification

| Check | Result |
| --- | --- |
| Stock UE 5.7 Development Editor build (`Build.bat`, `-NoUBA`) | Succeeded for Runtime and Editor modules |
| UE `Automation RunTests MtoULiveLink`, NullRHI | 80/80 Success, 0 failed/not run; 9 tests reported expected warnings |
| UE comparison coverage | Public Details action and Binding Actor path: repeated switching, material visibility, same Preview and session, nonzero Generated and Driver-only Morphs, new streamed pose during original display, body and Additional Parts, dirty input, deletion during original display, and cleanup |
| Pure Python tests and Maya 2024 mayapy host tests | 145/145 and 23/23 passed |
| Maya 2024 real peer with UE 5.7 | 1/1 passed for existing capture, explicit-refresh disconnect, and reconnect transport regression |
| Protocol corpus check; repository validator and tooling tests | Passed; 22/22 tooling tests |
| Maya and UE package dry runs | 0.8.0 packages resolved; 1 and 38 files respectively |

The headless Details test invokes the same handler as the button; it does not
simulate a physical Slate click. The real Maya peer checks existing transport
recovery in a disposable scene. The comparison stream test uses a synthetic
loopback peer and transient UE meshes, including an accepted nonzero Morph and
a Driver-only Morph. These checks cover synthetic behavior with the limitations
below; they do not establish production-character visual acceptance.

## Independent review

The [independent review comment](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/51#issuecomment-5855869248)
records the source review of commit `0ba74db7999ee732b5fc52c9144b995c7afc4cd4`
and a separate local verification on 2026-09-27:

- UE 5.7 `Build.bat UnrealEditor Win64 Development` for `unreal/ToolsLab.uproject`,
  with `-WaitMutex -NoHotReloadFromIDE -NoUBA`: Runtime and Editor modules built.
- Nine focused NullRHI Automation tests passed: Actor CharacterPartLifecycle
  and PreviewInputRestore; Editor Preview CharacterParts, FullCharacter,
  FullCharacterQualityCorpus and RefreshEndsSession; Preview Readiness and
  ReadinessGuards; Workflow Negotiation. There were no failures or unrun tests.
  Readiness emitted two synthetic-world destruction warnings (`World has no
  context`); the other eight tests had no test warnings.
- Ordinary Python pure tests passed 145/145; isolated Maya 2024 mayapy host
  tests passed 23/23, with the bundled dependency's `imp` deprecation warning.
  The full UE suite, cross-host peer, packaging and repository gates in the
  development table were not rerun by the independent review.

The review confirmed that negotiation uses the Generated Preview, Primary Driver
and enabled Additional Parts Morph-name union. ADR-0011 now consistently states
that contract, including the empty-intersection rejection condition.

## Review follow-up

The review's remaining automated coverage gap was closed by
`MtoULiveLink.Editor.Preview.ComparisonNaturalTick`, a latent regression that
runs in `GEditor`'s real editor world:

- The Binding actor is placed with a full-character Driver, a garment Preview,
  and one enabled Additional Part that owns a part-only Morph. The Model session
  negotiates a garment, Driver-only, and part-only Morph manifest, so the
  accepted set is exactly the Generated Preview, Primary Driver, and enabled
  part union.
- The Details handler performs every switch. Between a socket send and its
  observation nothing calls `Actor::Tick`, `TickAnimation`,
  `RefreshBoneTransforms`, or `World::Tick`: the editor main loop, the Live Link
  client's source update, and the plugin's realtime override advance the
  display. Control-plane steps (negotiation, Details clicks, invalidation) still
  pump `Source->Update` for determinism and draw no display conclusion.
- Four successive streamed poses are observed through the final displayed bones
  of the Generated Preview, the original Driver follower, and the part, and
  through their evaluated Morph weights (`USkinnedMeshComponent::MorphTargetWeights`)
  for the garment, Driver-only, and part-only names. Two poses arrive while the
  original garment is displayed, one before the switch, one after switching
  back, and the report records each reached state.
- Session id, Preview pointer, readiness, actor transform, and material-section
  exclusivity are asserted around every switch; the Driver-only and part-only
  Morphs are confirmed absent from the Generated Preview; a Preview input change
  made while the original garment is displayed still invalidates the comparison
  and clears both displays.

The synthetic fixtures build the two things a loaded character has and a
GeometryScript-built fixture does not: morph name lookup
(`InitMorphTargets`, because `RegisterMorphTarget(..., bInvalidateRenderData=false)`
leaves `MorphTargetIndexMap` unbuilt) and morph-target curve metadata, which
FBX import writes to the Skeleton by default. The engine resolves streamed Morph
values through both, so without them the fixtures would not represent an
imported asset and the evaluated weights would stay at zero.

The regression places the only binding actor of the editor world, and the plugin
refuses a Model connection with `MULTIPLE_BINDING_ACTORS` while any other placed
editor-world actor exists. A world destroyed by an earlier test is collected
only on the next garbage collection, so the setup collects before it asserts
that no other placed editor-world binding actor survives; no existing test was
changed for this.

### Review follow-up verification

| Check | Result |
| --- | --- |
| UE 5.7 Development Editor build (`Build.bat UnrealEditor Win64 Development unreal/ToolsLab.uproject -WaitMutex -NoHotReloadFromIDE -NoUBA`) | Succeeded; Runtime and Editor modules relinked |
| `Automation RunTests MtoULiveLink.Editor.Preview.ComparisonNaturalTick`, NullRHI | 1/1 Success, 0 errors, 0 warnings; the report records the four reached poses with the evaluated Morph weights of the Generated Preview, the original Driver follower, and the part |
| `Automation RunTests MtoULiveLink`, NullRHI | 84/84 Success, 0 failed or not run; the same 9 tests as before carry their expected warnings |
| `python tools/validate_repository.py` | `ok: True`, no errors or warnings |
| `python -m unittest discover -s tests -q` | 22 passed, 1 skipped |

The focused run and the full suite were both executed; the suite is what proves
the regression is order-independent, since the earlier single-test run could not
expose the pending editor world described above.

## Final independent acceptance

Maya 2024 and UE 5.7.4 (CL 51494982) loaded the supplied C01 scene and
Backups project assets through a disposable host. The installed project plugin
was not the review baseline, so the host used a separate copy of current source.
Only the temporary CharacterAcceptance harness was extended; product source,
Maya scene, Binding and character assets were not edited or saved.

The transient Binding used `SK_C01_CombineBody_Clothes_05`,
`SM_C01_Clothes_05`, and enabled Head and Hair parts. Automatic source selection
correctly rejected overlapping nail/shoe regions. The documented manual-slot
path resolved this asset configuration using imported slots
`M_C01_Clothes_16`, `M_C01_Clothes_05`, and `M_C01_Clothes05_Shoes`.
Refresh reached Ready with complete matched Preview coverage.

- Actual Maya Model transport drove three poses, with a Generated/Original
  pair captured for each pose and five consecutive Details-handler switches.
  Each switch retained the same Maya session object, Generated mesh pointer,
  readiness and Actor transform. It did not rebuild or reconnect.
- All material slots were checked: Generated and original garment slots were
  mutually exclusive, and body, nails and enabled parts remained visible.
- The real garment Morph
  `SM_C01_Clothes_05_Jacket__Shoulder_R_RotY_plus_0_35` evaluated to 0.35 on
  both Generated and original Driver at all six observations. Checks read
  `ActiveMorphTargets` / `MorphTargetWeights`, not only the assigned value.
- Final display transforms were checked against 811 required published bones
  across all owning components, including the original follower and parts.
  Maximum displayed position error was 0.000022981 cm (tolerance 0.1 cm).
  Follower transforms were read with the engine's bone-transform API because
  followers share their leader's pose rather than owning an evaluated array.
- Six 1392 x 837 BaseColor screenshots were inspected. Each pair retained the
  same camera and scene; head, hair, body and clothing remained composed.
  The images show the original and generated surfaces under the same pose.
- An input change during Original display released the cached Preview and
  disabled comparison. A failed refresh did not restore stale geometry.
  Restoring the input and explicitly refreshing rebuilt a usable Preview;
  deletion during Original display then cleared comparison and Generated data.

Independent checks in this acceptance run:

| Check | Result |
| --- | --- |
| Current Runtime/Editor source plus disposable acceptance harness, Development Editor build | Passed |
| Unmodified `ComparisonNaturalTick`, NullRHI | 1/1 passed, no errors or warnings; Generated/Driver-only/part-only evaluated Morphs and four naturally observed poses |
| Unmodified `Actor.PreviewInputRestore`, `Editor.Preview.CharacterParts`, `Editor.Preview.RefreshEndsSession`, `Preview.Readiness`, `Preview.ReadinessGuards`, `Workflow.Negotiation` | 6/6 passed; Readiness has the existing synthetic-world warnings |
| Production CharacterAcceptance, D3D12 rendering and real Maya peer | 1/1 passed, no errors; 167 warnings all concern existing assets saved with an empty engine version |

The Maya standalone peer runs production scene capture, controller and socket
code, pumping Maya callbacks on its main thread. UE display evaluation runs
through the editor loop. The harness invokes the same comparison handler as
Details; it does not claim physical Slate clicks. The natural-tick regression
separately proves updates without explicit display-tick calls, including
Driver-only and part-only Morphs. No full 84-test suite, Python suite or package
gates were rerun in this final phase: the changes being accepted add UE test
coverage, and no product source was changed here.

Reproduction harness, fixture, logs, report JSON and screenshots are retained
outside the repository in `../.tmp/issue51-acceptance/`; the accepted rendered
run is `final/`, `final-report/`, and `final.log`. No new branch, commit, PR or
push was created. Existing unrelated working-tree changes were preserved.
