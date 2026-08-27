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

Compatibility with third-party Unreal Engine 5.7 builds is not guaranteed.

## Installation

1. Copy `maya/MtoULiveLink/scripts/MtoULiveLink.py` to a Maya scripts directory,
   or run it directly in Maya's Python Script Editor.
2. Copy `unreal/MtoULiveLink/` to `<Project>/Plugins/MtoULiveLink/`.
3. Compile the Unreal project, enable **Live Link** and **MtoU_LiveLink**, then
   restart Unreal Editor.

Install the Maya and Unreal components from the same release.

## Usage

1. In Unreal, create an **MtoU_LiveLink Binding**, assign its **Driver Skeletal
   Mesh**, place one MtoU_LiveLink Binding Actor in the level, and assign the
   Binding to it.
   The actor's plugin-owned display component bypasses the Driver Skeletal
   Mesh's Post Process Anim Blueprint while displaying evaluated Maya data;
   the Driver asset and other production components keep their own behavior.
2. In Maya, run `MtoULiveLink.py`, select one deformation root joint, and click
   **Set Character**. Confirm the detected Display controller, outfit, scene
   rate, and transmission cap.
3. Choose **Animation** or **Model**, then click **Connect**.
4. In **Animation**, pose, scrub, or play in Maya for live preview. To review a
   captured range, choose **Cached Playback** and click **Capture and Play**.
5. In **Model**, also assign the garment **Preview Static Mesh** in Unreal and
   click **Refresh Preview** before connecting. Use **Transfer BS** to include
   matching BlendShapes.
6. Reconnect after changing the outfit, workflow, or **Transfer BS** setting.
