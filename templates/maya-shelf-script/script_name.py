"""One-line description of the shelf script."""

import maya.cmds as cmds


def run():
    """Execute the script's main operation."""
    selection = cmds.ls(selection=True, long=True) or []
    if not selection:
        cmds.warning("Script Name: select at least one object.")
        return

    cmds.inViewMessage(
        assistMessage="Script Name: validated {0} item(s).".format(len(selection)),
        position="midCenter",
        fade=True,
    )


if __name__ == "__main__":
    run()
