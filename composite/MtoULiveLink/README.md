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
   Mesh**, place one MtoU_LiveLink Binding Actor in the level, and assign the
   Binding to it.
   The actor's plugin-owned display component bypasses the Driver Skeletal
   Mesh's Post Process Anim Blueprint while displaying evaluated Maya data;
   the Driver asset and other production components keep their own behavior.
   Use the **MtoU** Details section to see only the plugin controls. After
   Refresh, **Modified parts** lists one Preview material slot per line for the
   parts that replace Driver geometry; internal triangle, timing, and quality
   metrics stay out of the artist-facing panel. **Delete Preview** releases the
   generated mesh and immediately restores the Driver Skeletal Mesh display.
2. In Maya, run `MtoULiveLink.py`, select one deformation root joint, and click
   **Set Character**. Confirm the detected Display controller, outfit, scene
   rate, and transmission cap.
3. Choose **Animation** or **Model**, then click **Connect**.
4. In **Animation**, pose, scrub, or play in Maya for live preview. To review a
   captured range, choose **Cached Playback** and click **Capture and Play**.
5. In **Model**, also assign the garment **Preview Static Mesh** in Unreal and
   click **Refresh Preview** before connecting. Use **Transfer BS** to include
   matching BlendShapes. For a full-character Driver, Model preview keeps its
   body, face, hair, and other non-garment material slots visible while the
   Generated Preview replaces the resolved original garment slots. Driver-only
   face and hair Morph Targets continue to receive matching Maya curves. Keep
   garment and non-garment geometry in separate imported material slots;
   Refresh reports a shared slot instead of hiding part of the character.
6. Reconnect after changing the outfit, workflow, or **Transfer BS** setting.
