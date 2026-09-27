# Issue #51: original garment and Generated Preview comparison

Date: 2026-09-27. Source development is complete on
`codex/mtou-preview-workflow`; production-character visual acceptance is
pending. Issue #51 remains open for the owner to verify it in a company scene.

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

## Local verification

| Check | Result |
| --- | --- |
| Stock UE 5.7 Development Editor build (`Build.bat`, `-NoUBA`) | Succeeded for Runtime and Editor modules |
| UE `Automation RunTests MtoULiveLink`, NullRHI | 80/80 Success, 0 failed/not run; 9 tests reported expected warnings |
| UE comparison coverage | Public Details action and Binding Actor path: repeated switching, material visibility, same Preview and session, nonzero Generated and Driver-only Morphs, new streamed pose during original display, body and Additional Parts, dirty input, deletion during original display, and cleanup |
| Maya 2024 pure Python and mayapy host tests | 145/145 and 23/23 passed |
| Maya 2024 real peer with UE 5.7 | 1/1 passed for existing capture, explicit-refresh disconnect, and reconnect transport regression |
| Protocol corpus check; repository validator and tooling tests | Passed; 22/22 tooling tests |
| Maya and UE package dry runs | 0.8.0 packages resolved; 1 and 38 files respectively |

The headless Details test invokes the same handler as the button; it does not
simulate a physical Slate click. The real Maya peer checks existing transport
recovery in a disposable scene. The comparison stream test uses a synthetic
loopback peer and transient UE meshes, including an accepted nonzero Morph and
a Driver-only Morph. These checks establish local behavior without using a
production character.

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
