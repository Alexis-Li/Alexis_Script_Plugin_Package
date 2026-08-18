# MtoU_LiveLink

[中文说明](README_CN.md)

## Introduction

MtoU_LiveLink is a composite Maya and Unreal plugin for locally previewing one
evaluated Maya deformation skeleton and its matching BlendShapes in Unreal
Live Link. Its two components communicate over a local loopback connection
while remaining independently installable and packageable.

## Supported Versions

- Windows 64-bit
- Autodesk Maya 2022.4
- Stock Unreal Editor 5.7.4

Compatibility with third-party Unreal Engine 5.7 builds is not claimed.

## Installation

Install the matching component in each host:

1. Maya: copy `maya/MtoULiveLink/scripts/MtoULiveLink.py` to a Maya scripts
   directory, or run it directly in Maya's Python Script Editor.
2. Unreal: copy the complete `unreal/MtoULiveLink/` directory to
   `<Project>/Plugins/MtoULiveLink/`. The installed descriptor must be
   `<Project>/Plugins/MtoULiveLink/MtoULiveLink.uplugin`.
3. Compile the Unreal project, enable **Live Link** and **MtoU_LiveLink**, and
   restart Unreal Editor.

## Usage

1. In Unreal, create an **MtoU_LiveLink Binding**, assign its **Skeletal Mesh**,
   and keep exactly one binding actor in the level.
2. In Maya, run `MtoULiveLink.py`, select exactly one deformation root, and
   select **Set Character**. The tool finds the character's `Display_ctrl` and
   Clothes enum; use the manual Display button if discovery is ambiguous.
   If duplicate transmitted bone names are found, use **Select Duplicate
   Bones** in the error dialog or main window to select every conflicting joint
   by its full DAG path and locate it in the Outliner.
   Duplicates do not fail immediately: Unreal maps a uniquely numeric-suffixed
   imported bone below the already matched parent, warns on success, and rejects
   an ambiguous mapping.
3. Confirm the displayed outfit and scene rate, then select **Connect**. The
   stream samples at the exact Maya scene rate from 1 through 60 fps.
4. Pose, play, or scrub in Maya. Changing the Clothes enum disconnects the
   session; replace the Unreal binding actor with the new outfit and reconnect.

The receiver preserves each placed actor transform and does not create an
Animation Sequence. The binding actor updates its animation continuously in
Unreal Editor. While connected, the plugin temporarily forces level viewports
into realtime mode and restores each viewport's prior setting on disconnect.
Disconnecting clears the last streamed frame so the mesh returns to its
reference pose instead of retaining a stale pose. Same-named BlendShapes
on separate skinned mesh parts are sent as one Unreal curve when their evaluated
values agree. If those values differ, sampling stops and identifies every
conflicting Maya plug. Maya- and Unreal-only BlendShape names are non-blocking
warnings. Protocol version 2 requires matching Maya and Unreal 0.2.0 components.
