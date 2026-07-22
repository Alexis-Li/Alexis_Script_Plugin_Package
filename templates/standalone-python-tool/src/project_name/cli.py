"""Command-line entry point."""

import argparse


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Emit JSON output.")
    parser.parse_args(argv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
