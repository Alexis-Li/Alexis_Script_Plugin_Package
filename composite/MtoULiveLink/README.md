# MtoU_LiveLink

[中文说明](README_CN.md)

## Introduction

MtoU_LiveLink is a composite Maya and Unreal plugin for locally previewing one
evaluated Maya character in Unreal Live Link. Its two components communicate
over the documented loopback protocol while remaining independently
installable and packageable.

## Supported Versions

- Windows 64-bit
- Autodesk Maya 2022.4
- Stock Unreal Editor 5.7.4

Compatibility with third-party Unreal Engine 5.7 builds is not claimed.

## Installation

Install only the component needed by each host:

1. Maya: use the files under
   [`maya/MtoULiveLink/`](maya/MtoULiveLink/README.md). Copy
   `scripts/MtoULiveLink.py` to a Maya scripts directory or run it directly in
   Maya's Python Script Editor.
2. Unreal: copy the complete
   [`unreal/MtoULiveLink/`](unreal/MtoULiveLink/README.md) directory to
   `<Project>/Plugins/MtoULiveLink/`. The installed descriptor must be
   `<Project>/Plugins/MtoULiveLink/MtoULiveLink.uplugin`.
3. Compile the Unreal project, enable **Live Link** and **MtoU_LiveLink**, and
   restart Unreal Editor.

## Usage

In Unreal, create an **MtoU_LiveLink Binding**, assign its Skeletal Mesh, and
drag it into the level. In Maya, select exactly one deformation root, run
`MtoULiveLink.py`, and select **Connect**. Pose, play, or scrub in Maya to drive
the Unreal Live Link subject; select **Disconnect** when finished.
