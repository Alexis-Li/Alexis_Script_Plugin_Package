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

1. In Unreal, create an **MtoU_LiveLink Binding** in the chosen Content Browser
   folder, assign its **Skeletal Mesh**, and drag the binding into the level.
2. In Maya, select exactly one deformation root, run `MtoULiveLink.py`, and
   select **Connect**.
3. Pose, play, or scrub in Maya to drive the `MtoU_Character` Live Link subject.
   Select **Disconnect** when finished, and reconnect after topology changes or
   either host restarts.

The receiver preserves each placed actor transform and does not create an
Animation Sequence. Same-named BlendShapes on separate skinned mesh parts are
sent as one Unreal curve when their evaluated values agree. If those values
differ, sampling stops and identifies every conflicting Maya plug.
