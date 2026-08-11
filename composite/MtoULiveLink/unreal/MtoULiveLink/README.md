# MtoULiveLink

[中文说明](README_CN.md)

## Introduction

MtoULiveLink receives one local Maya character as native Live Link animation
and curves while preserving each placed actor transform.

## Supported Versions

- Windows 64-bit
- Stock Unreal Editor 5.7.4

Compatibility with third-party Unreal Engine 5.7 builds is not yet claimed.

## Installation

1. Copy this complete `MtoULiveLink` directory to
   `<Project>/Plugins/MtoULiveLink/`. Confirm the descriptor is at
   `<Project>/Plugins/MtoULiveLink/MtoULiveLink.uplugin`.
2. Compile the project.
3. Enable **Live Link** and **MtoU_LiveLink**.
4. Restart Unreal Editor.

## Usage

1. Create an **MtoU_LiveLink Binding** in the chosen Content Browser folder.
2. Set its **Skeletal Mesh** to an existing asset.
3. Drag the binding into the level.
4. Connect from Maya.

No Animation Sequence is created.
