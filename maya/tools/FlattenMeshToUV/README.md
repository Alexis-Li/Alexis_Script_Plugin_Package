# Flatten Mesh To UV

[中文说明](README_CN.md)

## Introduction

Flatten Mesh To UV creates a polygon mesh on the XY plane from the selected
mesh's current UV set. It splits UV seams while preserving topology and UV
coordinates.

## Supported Versions

Python 2 and Python 3. Tested by the author in Maya 2020, 2022, and 2024.

## Installation

1. Copy `plug-ins/FlattenMeshToUV.py` to Maya's `plug-ins` directory.
2. Load `FlattenMeshToUV.py` in Maya's Plug-in Manager. Auto load is optional.

## Usage

1. Select one polygon mesh and activate the UV set to convert.
2. Run `scripts/RunFlattenMeshToUV.py` in Maya's Python Script Editor.
3. Save the launch code to a shelf button if needed.
