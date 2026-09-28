# Official Live Link capability review for a UE to Maya camera

Reviewed 2026-09-28 against public sources only; no Autodesk or Epic plugin was
installed in the hosts used for the prototype, and no official code was copied.
Everything below is either quoted or directly readable in the linked source.

## What exists officially

| Capability | Direction | Evidence |
| --- | --- | --- |
| Unreal Live Link for Maya camera subject | Maya → UE | [MLiveLinkBaseCameraSubject.cpp](https://raw.githubusercontent.com/Autodesk/LiveLink/main/Source/Programs/MayaUnrealLiveLinkPlugin/Subjects/MLiveLinkBaseCameraSubject.cpp) builds `FLiveLinkCameraFrameData` with `Transform`, `Aperture` (= camera `fStop`), `AspectRatio`, `FieldOfView` (horizontal, degrees), `FocalLength`, `FocusDistance`, `ProjectionMode`, plus static film back width/height converted from inches to mm. |
| Unreal Live Link for Maya time sync | documented bi-directional | Autodesk help: "Sync Time: Enable this to sync Maya's Time Slider with Unreal's playhead. This relationship is bi-directional." The UE-side module converts `Sequencer::OnGlobalTimeChanged` into a message-bus event and guards against re-entrancy with `bSetGlobalTime` / `bIgnoreTimeChange` ([MayaLiveLinkTimelineSyncModule.cpp](https://raw.githubusercontent.com/Autodesk/LiveLink/main/Plugins/Runtime/MayaLiveLink/Source/MayaLiveLinkTimelineSync/Private/MayaLiveLinkTimelineSyncModule.cpp)). |
| Epic "Enable Camera Sync" | Maya → UE | Epic 5.7 documentation: "Enables syncing of the Unreal Editor camera with an external editor. Internally this looks at Live Link for a subject called `EditorActiveCamera`." The subject is built from Maya's active viewport camera ([MLiveLinkActiveCamera.cpp](https://raw.githubusercontent.com/Autodesk/LiveLink/main/Source/Programs/MayaUnrealLiveLinkPlugin/Subjects/MLiveLinkActiveCamera.cpp)). |
| Live Link roles in UE | external → UE | [Live Link in Unreal Engine 5.7](https://dev.epicgames.com/documentation/unreal-engine/live-link-in-unreal-engine?application_version=5.7) describes streams arriving *into* Unreal. |
| LiveLinkXR, Virtual Camera, Master Lockit | unrelated to Maya | [LiveLinkXR (Beta)](https://dev.epicgames.com/documentation/unreal-engine/livelinkxr-in-unreal-engine?application_version=5.7) streams XR device tracking; Virtual Camera is an app-driven virtual camera; neither is a Maya camera path. |

**No official UE → Maya camera path exists.** Epic's camera sync and Autodesk's
camera subject are both Maya → Unreal. The roadmap's earlier caution ("do not
treat Maya → UE Camera Sync as an existing reverse feature") is confirmed by
source and documentation.

## Directly reusable, adaptable, missing

**Directly reusable (ideas and contracts, not code):**

- The camera property set Autodesk chose: it is the same set of magnitudes this
  prototype had to transfer (transform, focal length, aperture, aspect, focus
  distance, projection mode, film back), which confirms the field list is the
  right one for a Maya camera.
- The split between static connection data (film back) and per-frame data
  (transform, focus, aperture), which the prototype mirrors in its `session`
  and `frame` messages.
- The re-entrancy guard pattern from the official time sync (`bSetGlobalTime` /
  `bIgnoreTimeChange`). The prototype makes the same property structural: it
  refuses any client message that tries to move Unreal time and records the
  attempt, so a loop cannot form even if a future client sends one.

**Adaptable with work:**

- Autodesk's Maya-side plugin is MIT licensed ([LICENSE.md](https://raw.githubusercontent.com/Autodesk/LiveLink/main/LICENSE.md),
  Copyright (c) 2022 Autodesk, Inc.), so its camera code could legally be
  adapted. It was not, because it transmits from Maya and targets a different
  Unreal version (see constraints), which makes an adaptation a larger change
  than writing the small receiving path the prototype needs.
- Maya camera semantics: aperture and film offsets are in inches, focal length
  in millimetres, focus distance in the scene's linear working unit, and the
  film fit modes are `0 fill, 1 horizontal, 2 vertical, 3 overscan`
  ([Maya 2024 camera node](https://help.autodesk.com/cloudhelp/2024/ENU/Maya-Tech-Docs/Nodes/camera.html)).
  The prototype implements this mapping itself and validates it by projecting
  known markers instead of trusting the attribute names.

**Missing:**

- A UE → Maya camera link: nothing official pushes a Cine Camera into Maya.
- Time sync without the Autodesk plugin installed in both hosts. The official
  path requires the plugin in Maya *and* Unreal
  ([installation](https://help.autodesk.com/cloudhelp/2024/ENU/UnrealLiveLink/files/UnrealLiveLink_installation_html.html)).
- Maya-side camera cuts or shot selection: Maya has no equivalent concept, so a
  resolved camera is the only thing that can be transferred.
- Image-level equivalence between the renderers. Epic's cinematic depth of
  field "closely matches real-world cameras" but is a procedural bokeh, and
  "adjusting the F-stop and Diaphragm does not control the light intensity"
  ([cinematic DOF](https://dev.epicgames.com/documentation/unreal-engine/cinematic-depth-of-field-in-unreal-engine));
  Maya's depth of field uses its own f-stop/focus distance/focus region model.
  No official statment of pixel parity exists, and the prototype does not claim
  one.
- Output-resolution integration. Movie Render Pipeline exposes several
  different resolutions: `GetDesiredOutputResolution` (the target, without
  overscan/tiling/aspect constraints), `GetBackbufferResolution` (what the
  renderer uses, including overscan and tiling), `GetOverscannedResolution`,
  and `GetOverscanCropRectangle`
  ([MoviePipelineLibrary 5.7](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/MoviePipelineLibrary?application_version=5.7)).
  The prototype takes the resolution as a prototype input and reports
  `resolution_source` instead of guessing which pipeline value applies.

## Version and licence constraints

- Autodesk's latest release is **v2.6.0 for Unreal 5.5**; there is no 5.6/5.7
  release, while Maya 2022–2025 (including Maya 2024) remain supported
  ([releases](https://github.com/Autodesk/LiveLink/releases)). Compiling its
  Unreal side against UE 5.7 was therefore not attempted and must not be
  assumed.
- The repository is MIT licensed; any reuse needs attribution. The prototype
  reuses no code from it.
- Epic's own documentation pages are written for whichever engine version the
  page defaults to; the 5.7 pages were used where available.

## What this means for the prototype

The prototype is a small, self-contained receiving path: Unreal evaluates its
own camera and publishes it, Maya applies it. It depends on no Autodesk or Epic
plugin, on no new host requirement, and on no product protocol change. Adopting
the official stack instead would require shipping and validating Autodesk's
plugin in both hosts for UE 5.7 first, which is exactly the kind of unverified
prerequisite the verification issue forbids assuming.
