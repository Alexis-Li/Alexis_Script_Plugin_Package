# NitroPoly

[中文说明](README_CN.md)

## Introduction

NitroPoly 4.0.4 is a Maya polygon-modeling toolkit for selection, topology,
pivots, connections, and edge-loop workflows.

## Supported Versions

Python 2 and Python 3. Tested by the author in Maya 2020, 2022, and 2024.

## Installation

`NitroPoly.py` is the only runtime file. Copy it to Maya's user `scripts`
directory. Do not copy or create a separate `NitroPolyStart.py` file.

## Usage

Save the following Python command directly in a Maya shelf button. This is
button content, not another script file:

```python
try:
    reload
except NameError:
    from importlib import reload

import NitroPoly
reload(NitroPoly)
NitroPoly.main()
```

For a temporary run without installation, open `NitroPoly.py` in a Python tab
of Maya's Script Editor and execute the complete file. This is an alternative
to copying the file and creating a shelf button; the two workflows are not
cumulative.
