# ZiSpread

[中文说明](README_CN.md)

## Introduction

`ZiSpread_Rebuilt.py` is a Python reconstruction of the original 64-bit C++
ZiSpread plug-in. The single-file version runs in both Python 2 and Python 3
Maya versions without requiring a different `.mll` file for each Maya release.

## Installation

No plug-in installation is required. Open `ZiSpread_Rebuilt.py`, copy its full
contents into the Python tab of Maya's Script Editor, and run it. You can also
save the full script to a shelf button.

## Usage

1. Select the edge loops you want to distribute evenly.
2. Run the script.
3. Hold the left or middle mouse button and drag horizontally in the viewport.
4. Release the mouse button to confirm the result.
5. Press Ctrl+Z to undo.
