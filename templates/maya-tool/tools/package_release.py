"""Package this Maya tool through the repository-level packager."""

import pathlib
import runpy
import sys


def main():
    project = pathlib.Path(__file__).resolve().parents[1]
    repository = next(parent for parent in project.parents if (parent / "tools").is_dir())
    sys.argv[1:1] = [project.name]
    runpy.run_path(str(repository / "tools" / "package_maya_tool.py"), run_name="__main__")


if __name__ == "__main__":
    main()
