"""Print development-install instructions; installation is intentionally manual."""

import pathlib


def main():
    module_dir = pathlib.Path(__file__).resolve().parents[1] / "package"
    print("Add this directory to MAYA_MODULE_PATH: {0}".format(module_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
