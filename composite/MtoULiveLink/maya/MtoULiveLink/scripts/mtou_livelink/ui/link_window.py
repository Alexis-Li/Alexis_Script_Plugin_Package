"""镜头联动 detail window: the authoritative linkage state of one session.

The window owns no state of its own: every value it renders comes from a
controller getter and every button sends a controller action, so the main card
and this window can never disagree about the mode, the target or the restore
outcome.
"""

from .. import mayaapi
from .layout import apply_panel_theme, grid_row, set_text
from .style import CARD_MARGIN, GRID_GAP, ROW_SPACING

LINK_WINDOW_NAME = "MtoULiveLinkLinkWindow"

# One window width that keeps the status grid readable beside the read-back
# values; the cards line up with the same gutter as the main panel.
WINDOW_WIDTH = 520

WINDOW_CONTENT = WINDOW_WIDTH - 2 * CARD_MARGIN

STATUS_COLUMN = (WINDOW_CONTENT - 2 * GRID_GAP) // 3


class LinkWindow(object):
    """Shows the live linkage status, recovery detail and read-back values."""

    def __init__(self, controller):
        self._controller = controller
        self._control_text = None
        self._mode_text = None
        self._target_text = None
        self._applied_text = None
        self._lag_text = None
        self._recovery_text = None
        self._detail_text = None
        self._target_eval_text = None
        self._applied_eval_text = None
        self._sample_text = None
        self._age_text = None
        self._count_text = None
        self._pending_text = None
        self._resource_text = None
        self._close_button = None
        self._card_decorations = []
        self._styled_controls = {}
        self._build()

    # --------------------------------------------------------------- build
    def _build(self):
        cmds = mayaapi.cmds
        cmds.window(
            LINK_WINDOW_NAME, title="MtoU 镜头联动",
            closeCommand=self._on_closed, sizeable=False, width=WINDOW_WIDTH,
            resizeToFitChildren=True)
        cmds.columnLayout(adjustableColumn=True, rowSpacing=ROW_SPACING,
                          columnAttach=("both", CARD_MARGIN))
        cmds.text(label="", height=1)

        status_card = cmds.frameLayout(
            label="联动状态", collapsable=False,
            marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN)
        cmds.rowLayout(
            numberOfColumns=3,
            columnWidth3=(STATUS_COLUMN, STATUS_COLUMN, STATUS_COLUMN),
            columnAttach3=("left", "left", "left"))
        cmds.columnLayout(adjustableColumn=False, rowSpacing=6)
        self._control_text = cmds.text(label="控制方：—", align="left")
        self._mode_text = cmds.text(label="模式：—", align="left")
        cmds.setParent("..")
        cmds.columnLayout(adjustableColumn=False, rowSpacing=6)
        self._target_text = cmds.text(label="目标时间：—", align="left")
        self._applied_text = cmds.text(label="最新完整应用：—", align="left")
        cmds.setParent("..")
        cmds.columnLayout(adjustableColumn=False, rowSpacing=6)
        self._lag_text = cmds.text(label="滞后：—", align="left")
        self._recovery_text = cmds.text(label="恢复结果：无", align="left")
        cmds.setParent("..")
        cmds.setParent("..")
        cmds.rowLayout(numberOfColumns=1, columnWidth1=WINDOW_CONTENT)
        self._detail_text = cmds.text(label="未进入镜头联动。", align="left",
                                      wordWrap=True, height=32)
        cmds.setParent("..")
        cmds.setParent("..")

        readback_card = cmds.frameLayout(
            label="读回值", collapsable=False,
            marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN - 3)
        cmds.columnLayout(adjustableColumn=False, rowSpacing=6)
        self._target_eval_text = cmds.text(label="目标求值：—", align="left")
        self._applied_eval_text = cmds.text(label="已应用求值：—", align="left")
        self._sample_text = cmds.text(label="样本序号：—", align="left")
        self._age_text = cmds.text(label="应用年龄：—", align="left")
        self._count_text = cmds.text(label="已应用 / 已拒绝：0 / 0", align="left")
        self._pending_text = cmds.text(label="在途目标：0", align="left")
        cmds.setParent("..")
        cmds.setParent("..")

        resource_card = cmds.frameLayout(
            label="逐资源恢复", collapsable=False,
            marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN - 3)
        cmds.rowLayout(numberOfColumns=1, columnWidth1=WINDOW_CONTENT)
        self._resource_text = cmds.text(
            label="没有需要恢复的资源。", align="left", wordWrap=True, height=54)
        cmds.setParent("..")
        cmds.setParent("..")

        cmds.formLayout(width=WINDOW_CONTENT)
        self._close_button = cmds.button(label="关闭窗口", height=34,
                                         command=lambda *_: self.close())
        grid_row([self._close_button], height=34 + GRID_GAP, top=GRID_GAP)
        cmds.setParent("..")

        cmds.showWindow(LINK_WINDOW_NAME)
        self._card_decorations, self._styled_controls = apply_panel_theme(
            LINK_WINDOW_NAME,
            (status_card, readback_card, resource_card),
            {self._close_button: "primary"})
        self.refresh()

    # ------------------------------------------------------------- actions
    def refresh(self):
        """Render the controller's current linkage snapshot."""
        status = self._controller.linkage_status()
        labels = status.get("labels") or {}
        # The status row is the main card's own labels: one source, no drift.
        set_text(self._control_text, labels.get("control"))
        set_text(self._mode_text, labels.get("mode"))
        set_text(self._target_text, labels.get("target"))
        set_text(self._applied_text, labels.get("applied"))
        set_text(self._lag_text, labels.get("lag"))
        set_text(self._recovery_text, labels.get("recovery"))
        set_text(self._detail_text, status.get("detail") or "—")

        set_text(self._target_eval_text, "目标求值：{0}".format(
            status.get("target") or "—"))
        set_text(self._applied_eval_text, "已应用求值：{0}".format(
            status.get("applied_eval_id") or "—"))
        set_text(self._sample_text, "样本序号：{0}".format(
            "—" if status.get("applied_sample_serial") is None
            else status.get("applied_sample_serial")))
        age = status.get("applied_age")
        set_text(self._age_text, "应用年龄：{0}".format(
            "—" if age is None else "{0:.1f} 秒".format(age)))
        set_text(self._count_text, "已应用 / 已拒绝：{0} / {1}".format(
            status.get("applied_count") or 0, status.get("refused_count") or 0))
        set_text(self._pending_text, "在途目标：{0}".format(
            (status.get("issued") or 0) + (status.get("pending") or 0)))
        records = status.get("recovery_records") or ()
        set_text(self._resource_text, "没有需要恢复的资源。" if not records
                 else "\n".join(records))

    def show(self):
        cmds = mayaapi.cmds
        if cmds is not None:
            cmds.showWindow(LINK_WINDOW_NAME)

    def close(self):
        cmds = mayaapi.cmds
        if cmds is not None and cmds.window(LINK_WINDOW_NAME, exists=True):
            cmds.deleteUI(LINK_WINDOW_NAME)

    # ------------------------------------------------------------- internals
    def _on_closed(self, *unused):
        del unused
        # The window's own close must never touch the linkage session; the
        # controller only drops the reference it kept for reuse.
        self._controller.close_link_window()
