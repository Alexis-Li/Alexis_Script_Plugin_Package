# Issue #51: original garment and Generated Preview comparison

Date: 2026-09-28. Source development is complete on
`codex/mtou-preview-workflow`. Independent review found no confirmed blocking
functional defect; its editor-tick and evaluated-Morph coverage gap is closed by
the natural-tick regression recorded below. Production-character visual
acceptance in the target project remains pending. Issue #51 remains open.

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
no protocol change was needed. Both host components identify as 0.8.0; the
wire protocol remains v8.

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

## Remaining acceptance

This computer has no company character or UE project assets. In the target
editor, use one real full-character or split-part Binding with nonzero Morphs:
connect the Maya Model workflow, pose and repeatedly switch the Details control,
inspect garment exclusivity, body and part visibility, and matching pose under
the same camera and lighting. Then modify an input, refresh or fail a refresh,
delete the Preview, and confirm comparison becomes unavailable without showing
stale generated geometry. Capture the required BaseColor viewport evidence for
the visual check. Keep Issue #51 open until that production-scene acceptance is
recorded.
