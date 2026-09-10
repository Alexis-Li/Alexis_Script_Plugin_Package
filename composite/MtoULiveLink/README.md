# MtoU_LiveLink

[中文说明](README_CN.md)

## Introduction

MtoU_LiveLink is a Maya and Unreal plugin for previewing evaluated Maya
skeleton animation and BlendShapes in Unreal Editor over a local Live Link
connection. It supports live animation preview, cached playback, and garment
model preview without creating animation or preview assets.

## Supported Versions

- Windows 64-bit
- Autodesk Maya 2022.4
- Stock Unreal Editor 5.7.4
- Topia Engine 5.7.4 (Win64 plugin build and editor loading verified)
- Unreal Editor only. Play In Editor (PIE) is not supported: PIE worlds are
  excluded from streaming target discovery.

Compatibility with other third-party Unreal Engine 5.7 builds is not guaranteed.

## Installation

1. Copy `maya/MtoULiveLink/scripts/MtoULiveLink.py` to a Maya scripts directory,
   or run it directly in Maya's Python Script Editor.
2. Copy `unreal/MtoULiveLink/` to `<Project>/Plugins/MtoULiveLink/`.
3. For stock Unreal, compile the Unreal project. For Topia Engine 5.7.4, close
   Unreal Editor, set `TOPIA_ENGINE_ROOT` to the directory containing `Engine`
   and `ATHENA_UPROJECT` to the target `.uproject`, then run the repository
   helper from the repository root:

   ```powershell
   pwsh ./tools/build_mtou_topia.ps1 `
     -EngineRoot $env:TOPIA_ENGINE_ROOT `
     -ProjectFile $env:ATHENA_UPROJECT `
     -Apply
   ```

   Without `-Apply`, the command only validates paths and previews the five
   generated files. Add `-Json` for machine-readable output. The Topia path
   stages all writable build state in a temporary directory and installs only
   `Binaries/Win64` under the copied MtoULiveLink plugin; it does not modify
   engine files or project source/configuration files.
4. Enable **Live Link** and **MtoU_LiveLink**, then restart Unreal Editor.

Install the Maya and Unreal components from the same release.

## Usage

1. In Unreal, create an **MtoU_LiveLink Binding**, assign its **Driver Skeletal
   Mesh**, then drag the Binding asset from the Content Browser into the level.
   This creates the MtoU_LiveLink Binding Actor and assigns the Binding
   automatically; the actor's Binding reference is not an editable setup field.
   The actor's plugin-owned display component bypasses the Driver Skeletal
   Mesh's Post Process Anim Blueprint while displaying evaluated Maya data;
   the Driver asset and other production components keep their own behavior.
   Use the **MtoU** Details section to see only the plugin controls. After
   Refresh, **Modified parts** lists one Preview material slot per line for the
   parts that replace Driver geometry; internal triangle, timing, and quality
   metrics stay out of the artist-facing panel. **Delete Preview** releases the
   generated mesh and immediately restores the Driver Skeletal Mesh display.
   Configure rendering once on **SkeletalMeshComponent**. The internal Driver
   display stays hidden from Details and inherits its Lighting Channels and
   Dynamic Inset Shadow setting when Model preview needs both meshes.
2. In Maya, run `MtoULiveLink.py`, select one deformation root joint, and click
   **Set Character**. Confirm the detected Display controller, outfit, scene
   rate, and transmission cap.
3. Choose **Animation** or **Model**, then click **Connect**.
4. In **Animation**, pose, scrub, or play in Maya for live preview. To review a
   captured range, choose **Cached Playback** and click **Capture and Play**.
   A capture or upload failure resumes Real-time Preview and shows the reason;
   a runtime playback failure keeps the cache so you can retry or leave cached
   mode. Captures stop as soon as the fixed 20,000-frame or 1 GiB cache limits
   would be crossed.
5. In **Model**, also assign the garment **Preview Static Mesh** in Unreal and
   click **Refresh Preview** before connecting. Use **Transfer BS** to include
   matching BlendShapes. With Transfer BS off, the session connects as a
   labelled bone-only diagnostic that is not valid for model acceptance. With
   Transfer BS on, an outfit that declares no BlendShapes is intentionally
   bone-driven and connects as Ready with an empty accepted set. In either
   case, the garment preview follows the skeleton and drives no Morph Target.
   For a full-character Driver, Model preview keeps its
   body, face, hair, and other non-garment material slots visible while the
   Generated Preview replaces the resolved original garment slots. Driver-only
   face and hair Morph Targets continue to receive matching Maya curves. Keep
   garment and non-garment geometry in separate imported material slots;
   Refresh reports a shared slot instead of hiding part of the character. A
   failed Refresh keeps Error readiness with no usable preview but restores the
   bound Driver display for inspection; Model connection stays blocked until a
   later Refresh succeeds.
   Auto uses a conservative topology-density limit: when the Driver has more
   than one connected region and the Preview has at least 8 triangles, selected
   Driver triangles must not exceed 1.70 times the Preview triangle count.
   Even an aligned, same-surface reduction can exceed this limit; the diagnostic
   does not prove duplicate or body geometry. Inspect the source, remove any
   duplicates, and separate mixed garment/body material slots. For an intended
   reduction, set **Driver Garment Slot Override** on the Binding to only the
   garment's distinct slots, then **Refresh Preview**. Manual selection still
   checks geometry, whole-Preview coverage, alignment, shared slots, and transfer
   quality. A usable yellow Warning still needs inspection before acceptance.
   Refresh runs synchronously and can block the Editor for seconds: the measured
   synthetic 27,744-triangle garment / 147,744-triangle Driver takes about
   1.6–3.1 seconds with 4–64 Driver Morphs on the recorded host. This is a measured
   corpus, not a universal size or latency guarantee; see the
   [geometry-scale measurements](../../docs/project-history/mtou-livelink/preview-refresh-geometry-scale.md).
6. Reconnect after changing the outfit, workflow, or **Transfer BS** setting.
   Clicking **Refresh Preview** also ends any active Animation or Model session
   before replacing the display. After a successful refresh, reconnect to
   negotiate against the refreshed preview.
   Changing the Driver Skeletal Mesh or Preview Static Mesh (including a
   reimport), deleting the Binding Actor, or unloading its Editor world ends
   the active streaming session and invalidates the current Preview revision:
   run **Refresh Preview** and connect again.
