"""Maya timeline, playback range and playback cap helpers."""

from .mayaapi import cmds

from .config import DEFAULT_PLAYBACK_CAP, PLAYBACK_CAP_CHOICES, PLAYBACK_CAP_OPTION_VAR
from .scene import require_maya


def normalize_playback_cap(value):
    if value is None:
        return DEFAULT_PLAYBACK_CAP
    text = str(value).strip().lower()
    if text in ("follow scene", "follow_scene", "scene"):
        return "Follow Scene"
    for choice in PLAYBACK_CAP_CHOICES[1:]:
        if text in (choice.lower(), choice.split()[0]):
            return choice
    return DEFAULT_PLAYBACK_CAP


def playback_cap_fps(value):
    value = normalize_playback_cap(value)
    if value == "Follow Scene":
        return None
    return float(value.split()[0])


def save_playback_cap(value):
    value = normalize_playback_cap(value)
    if cmds is not None:
        try:
            cmds.optionVar(stringValue=(PLAYBACK_CAP_OPTION_VAR, value))
        except (AttributeError, RuntimeError, TypeError):
            pass
    return value


def load_playback_cap():
    value = DEFAULT_PLAYBACK_CAP
    if cmds is not None:
        try:
            if cmds.optionVar(exists=PLAYBACK_CAP_OPTION_VAR):
                value = cmds.optionVar(query=PLAYBACK_CAP_OPTION_VAR)
        except (AttributeError, RuntimeError, TypeError):
            value = DEFAULT_PLAYBACK_CAP
    value = normalize_playback_cap(value)
    return save_playback_cap(value)


def maya_is_playing():
    if cmds is None:
        return False
    try:
        return bool(cmds.play(query=True, state=True))
    except (AttributeError, RuntimeError, TypeError):
        return False


class MayaTimeline(object):
    """Small adapter around Maya's current frame and Playback Range APIs."""

    def current_frame(self):
        require_maya()
        return cmds.currentTime(query=True)

    def playback_range(self):
        require_maya()
        return (
            cmds.playbackOptions(query=True, min=True),
            cmds.playbackOptions(query=True, max=True),
        )

    def is_playing(self):
        return maya_is_playing()

    def stop(self):
        if self.is_playing():
            cmds.play(state=False)

    def set_frame(self, frame):
        require_maya()
        cmds.currentTime(frame, edit=True)
