"""对象配对 detail window: the object set this panel streams to Unreal.

The window owns no state: the primary character, the second object and every
button's enablement come from controller getters, and every button sends a
controller action. It shows which Maya roots form the set, what the second
object needs in Unreal and what the set allows; the set revision and the
negotiation details are in 诊断详情.
"""

from .. import mayaapi
from .layout import (
    apply_panel_theme,
    elide_text,
    grid_row,
    release_panel_theme,
    set_enabled,
    set_text,
    set_tooltip,
    text_units,
)
from .style import CARD_MARGIN, GRID_GAP, ROW_SPACING

OBJECTS_WINDOW_NAME = "MtoULiveLinkObjectsWindow"

WINDOW_WIDTH = 520

WINDOW_CONTENT = WINDOW_WIDTH - 2 * CARD_MARGIN

# A card's body sits inside its margins and Maya's one-pixel frame; a wider
# label widens the whole window past WINDOW_WIDTH.
CARD_BODY = WINDOW_CONTENT - 2 * (CARD_MARGIN + 1)

# The longest line (a CJK character counts two) a card label shows; a longer
# DAG path is shortened in the middle and the tooltip keeps it whole.
LINE_UNITS = 68

OBJECTS_HINT = (
    "副对象是主角色旁边的独立骨骼道具或第二个角色，与主角色同时实时预览和镜头联动。"
    "在 UE 中把第二个绑定 Actor 配对为“副对象”，并放在与主角色相同的位置。"
    "缓存播放和模型工作流只支持单个角色。")


def object_line(prefix, value):
    """Return ``(label, tooltip)`` for one root or outfit line."""
    value = "—" if value in (None, "") else str(value)
    budget = LINE_UNITS - text_units(prefix)
    return prefix + elide_text(value, budget, middle=True), prefix + value


class ObjectsWindow(object):
    """Shows the object set and offers setting or removing the second object."""

    def __init__(self, controller):
        self._controller = controller
        self._primary_text = None
        self._secondary_text = None
        self._secondary_detail_text = None
        self._state_text = None
        self._set_button = None
        self._remove_button = None
        self._close_button = None
        self._card_decorations = []
        self._styled_controls = {}
        self._closed = False
        self._build()

    # --------------------------------------------------------------- build
    def _build(self):
        cmds = mayaapi.cmds
        cmds.window(
            OBJECTS_WINDOW_NAME, title="MtoU 对象配对",
            closeCommand=self._on_closed, sizeable=False, width=WINDOW_WIDTH,
            resizeToFitChildren=True)
        cmds.columnLayout(adjustableColumn=True, rowSpacing=ROW_SPACING,
                          columnAttach=("both", CARD_MARGIN))
        cmds.text(label="", height=1)

        set_card = cmds.frameLayout(
            label="对象集合", collapsable=False,
            marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN - 3)
        cmds.columnLayout(adjustableColumn=False, rowSpacing=6)
        self._primary_text = cmds.text(label="主角色：—", align="left", width=CARD_BODY)
        self._secondary_text = cmds.text(label="副对象：未设置", align="left", width=CARD_BODY)
        self._secondary_detail_text = cmds.text(
            label="副对象骨骼：—", align="left", width=CARD_BODY)
        self._state_text = cmds.text(label="状态：只连接主角色", align="left", width=CARD_BODY)
        cmds.setParent("..")
        cmds.setParent("..")

        hint_card = cmds.frameLayout(
            label="使用说明", collapsable=False,
            marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN - 3)
        cmds.rowLayout(numberOfColumns=1, columnWidth1=WINDOW_CONTENT)
        cmds.text(label=OBJECTS_HINT, align="left", wordWrap=True, height=58,
                  width=CARD_BODY)
        cmds.setParent("..")
        cmds.setParent("..")

        cmds.formLayout(width=WINDOW_CONTENT)
        self._set_button = cmds.button(
            label="设为副对象", height=34,
            command=lambda *_: self._controller.set_secondary_object())
        self._remove_button = cmds.button(
            label="移除副对象", height=34,
            command=lambda *_: self._controller.remove_secondary_object())
        grid_row([self._set_button, self._remove_button],
                 height=34 + GRID_GAP, top=GRID_GAP)
        cmds.formLayout(width=WINDOW_CONTENT)
        self._close_button = cmds.button(
            label="关闭窗口", height=34,
            command=lambda *_: self._controller.close_objects_window())
        grid_row([self._close_button], height=34 + GRID_GAP, top=GRID_GAP)
        cmds.setParent("..")
        cmds.setParent("..")

        cmds.showWindow(OBJECTS_WINDOW_NAME)
        self._card_decorations, self._styled_controls = apply_panel_theme(
            OBJECTS_WINDOW_NAME,
            (set_card, hint_card),
            {self._set_button: "primary"})
        self.refresh()

    # ------------------------------------------------------------- actions
    def refresh(self):
        """Render the controller's current object set and actions."""
        if self._closed:
            return
        state = self._controller.objects_state()
        label, tooltip = object_line("主角色：", state.get("primary"))
        set_text(self._primary_text, label, tooltip=tooltip)
        label, tooltip = object_line("副对象：", state.get("secondary") or "未设置")
        set_text(self._secondary_text, label, tooltip=tooltip)
        set_text(self._secondary_detail_text, state.get("secondary_detail") or "副对象骨骼：—")
        set_text(self._state_text, "状态：{0}".format(state.get("state") or "—"),
                 tooltip=state.get("state_tooltip") or "")

        controls = self._controller.objects_window_controls()
        for button, control in ((self._set_button, controls.get("set")),
                                (self._remove_button, controls.get("remove"))):
            if not isinstance(control, (list, tuple)) or len(control) != 2:
                continue
            set_enabled(button, control[0])
            set_tooltip(button, control[1])

    def exists(self):
        """True while this instance's Maya window is still open."""
        cmds = mayaapi.cmds
        return bool(not self._closed and cmds is not None
                    and cmds.window(OBJECTS_WINDOW_NAME, exists=True))

    def show(self):
        cmds = mayaapi.cmds
        if cmds is not None:
            cmds.showWindow(OBJECTS_WINDOW_NAME)

    def close(self):
        """Delete this window; Maya then sends its close notification."""
        if self._closed:
            return
        decorations = self._card_decorations
        self._forget_widgets()
        release_panel_theme(decorations)
        cmds = mayaapi.cmds
        if cmds is not None and cmds.window(OBJECTS_WINDOW_NAME, exists=True):
            cmds.deleteUI(OBJECTS_WINDOW_NAME)

    # ------------------------------------------------------------- internals
    def _forget_widgets(self):
        self._closed = True
        self._card_decorations = []
        self._styled_controls = {}

    def _on_closed(self, *unused):
        """Maya is closing this window; its close event is still running.

        Closing this window only releases the window: the object set, the
        connection and the second object stay as they are.
        """
        del unused
        self._forget_widgets()
        self._controller.objects_window_closed(self)
