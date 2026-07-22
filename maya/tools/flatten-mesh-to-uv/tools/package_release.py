"""Invoke the repository Maya packager for Flatten Mesh To UV."""

import pathlib
import runpy
import sys


if __name__ == "__main__":
    repository = pathlib.Path(__file__).resolve().parents[4]
    sys.argv[1:1] = ["flatten-mesh-to-uv"]
    runpy.run_path(str(repository / "tools" / "package_maya_tool.py"), run_name="__main__")
