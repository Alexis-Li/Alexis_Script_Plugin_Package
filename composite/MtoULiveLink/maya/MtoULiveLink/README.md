# MtoULiveLink

[中文说明](README_CN.md)

## Introduction

MtoULiveLink streams one evaluated Maya deformation skeleton and its matching
BlendShapes to the local Unreal `MtoU_Character` Live Link subject without
editing or exporting the scene.

## Supported Versions

- Windows 64-bit
- Autodesk Maya 2022.4 only

## Installation

Copy `scripts/MtoULiveLink.py` to a Maya scripts directory, or open the file
directly in Maya's Python Script Editor.

## Usage

1. Place and configure the Unreal binding actor first.
2. In Maya, select exactly one deformation root.
3. Execute `scripts/MtoULiveLink.py`.
4. Select **Connect**.
5. Pose, play, or scrub the Maya scene.
6. Select **Disconnect** when finished.
7. Reconnect after topology edits or after either host restarts.

Same-named BlendShapes on separate skinned mesh parts are sent as one Unreal
curve when their evaluated values agree. If those values differ, sampling stops
and identifies every conflicting Maya plug.
