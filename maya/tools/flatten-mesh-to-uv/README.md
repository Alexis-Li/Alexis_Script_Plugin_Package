# Flatten Mesh To UV

[中文说明](README_CN.md)

## Introduction

Flatten Mesh To UV creates a new polygon mesh on the XY plane from the selected
mesh's current UV set. UV seams are split while the original topology and UV
coordinates are preserved.

The tool supports Python 2 and Python 3 and has been tested by the author in
Maya 2020, 2022, and 2024.

## Installation

1. Copy `package/FlattenMeshToUV/plug-ins/FlattenMeshToUV.py` to
   `Documents\maya\20xx\plug-ins`.
2. Open Maya's Plug-in Manager and load `FlattenMeshToUV.py`. Enabling both
   Loaded and Auto load is recommended.

## Usage

1. Select one polygon mesh in Maya.
2. Make sure its current UV set is the one you want to convert.
3. Run the contents of
   `package/FlattenMeshToUV/scripts/FlattenMeshToUV_Start.py` in the Python tab
   of Maya's Script Editor.
4. Save the launch code to a shelf button if you use it frequently.
