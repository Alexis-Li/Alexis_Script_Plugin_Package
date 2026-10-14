"""场景参考 detail window: one Unreal handoff offer and its import outcome.

Like the linkage window this one owns no state: the offered handoff, the frozen
budgets, the last report and even the button enablement come from controller
getters, and every button sends a controller action.
"""

from .. import mayaapi
from .layout import apply_panel_theme, grid_row, set_enabled, set_text, set_tooltip
from .style import CARD_MARGIN, GRID_GAP, ROW_SPACING

REFERENCE_WINDOW_NAME = "MtoULiveLinkReferenceWindow"

WINDOW_WIDTH = 520

WINDOW_CONTENT = WINDOW_WIDTH - 2 * CARD_MARGIN

_MIB = 1024 * 1024


def _scope_summary(offer):
    scope = offer.get("scope")
    if not isinstance(scope, dict) or not scope:
        return "—"
    parts = [str(scope.get("level") or "—")]
    sublevels = scope.get("sublevels")
    if isinstance(sublevels, (list, tuple)):
        parts.append("子关卡 {0}".format(len(sublevels)))
    completeness = scope.get("completeness")
    if completeness:
        parts.append(str(completeness))
    unloaded = scope.get("unloaded_sublevels")
    if isinstance(unloaded, (list, tuple)) and unloaded:
        parts.append("未加载 {0}".format(len(unloaded)))
    return "｜".join(parts)


def _counts_summary(counts):
    if not isinstance(counts, dict) or not counts:
        return "—"
    parts = []
    for key, label in (("objects", "对象"), ("triangles", "三角面"), ("bytes", "字节")):
        if counts.get(key) is not None:
            parts.append("{0} {1}".format(label, counts.get(key)))
    return "｜".join(parts) if parts else "—"


def _count_label(value):
    return "{0:,}".format(int(value))


def _residual_text(residual):
    """One readable line for a report's residual payload, whatever its shape."""
    if not residual:
        return "无"
    if isinstance(residual, dict):
        entries = ["{0}：{1}".format(key, value)
                   for key, value in sorted(residual.items()) if value]
        return "；".join(entries) if entries else "无"
    if isinstance(residual, (list, tuple)):
        return "；".join(str(value) for value in residual) if residual else "无"
    return str(residual)


def _report_lines(outcome):
    """The report card's lines for the last import, removal or recovery."""
    action = outcome.get("action")
    report = outcome.get("report")
    if not isinstance(report, dict):
        return ["还没有导入记录。"]
    if action == "import":
        lines = ["导入：{0}".format(
            "已接受" if report.get("accepted") else "已拒绝")]
        detail = report.get("detail")
        if detail:
            lines.append("说明：{0}".format(detail))
        lines.append("丢弃节点：{0}".format("是" if report.get("discarded") else "否"))
        lines.append("回滚：{0}".format("是" if report.get("rolled_back") else "否"))
        lines.append("残留：{0}".format(_residual_text(report.get("residual"))))
        counts = report.get("counts")
        if isinstance(counts, dict):
            lines.append("计数：{0}".format(_counts_summary(counts)))
        container = report.get("container")
        if isinstance(container, dict) and container.get("name"):
            lines.append("容器：{0}".format(container["name"]))
        return lines
    if action == "remove":
        return [
            "移除参考：{0}".format("已完成" if report.get("removed") else "无需移除"),
            "说明：{0}".format(report.get("detail") or "—"),
            "移除节点：{0}".format(report.get("nodes_removed") or 0),
            "残留：{0}".format(_residual_text(report.get("residual"))),
        ]
    if action == "recover":
        lines = ["恢复：已完成"]
        for key, label in (
                ("restored_previous_reference", "已恢复上一次参考"),
                ("retired_leftovers_removed", "已清理遗留命名空间"),
                ("interrupted_container_removed", "已移除中断的容器"),
                ("finished_interrupted_container", "已完成中断的导入"),
                ("stale_staging_namespaces", "过期暂存命名空间"),
                ("skipped_namespaces", "已跳过的命名空间"),
                ("partial_cleanups", "部分清理")):
            value = report.get(key)
            if value:
                lines.append("{0}：{1}".format(
                    label, len(value) if isinstance(value, (list, tuple)) else value))
        events = report.get("events")
        if isinstance(events, (list, tuple)) and events:
            lines.append("事件：{0}".format("；".join(str(event) for event in events)))
        return lines
    return ["还没有导入记录。"]


class ReferenceWindow(object):
    """Shows the offered handoff, the frozen budgets and the last report."""

    def __init__(self, controller):
        self._controller = controller
        self._transfer_text = None
        self._scope_text = None
        self._digest_text = None
        self._counts_text = None
        self._space_text = None
        self._budget_text = None
        self._report_text = None
        self._import_button = None
        self._remove_button = None
        self._recover_button = None
        self._close_button = None
        self._card_decorations = []
        self._styled_controls = {}
        self._build()

    # --------------------------------------------------------------- build
    def _build(self):
        cmds = mayaapi.cmds
        cmds.window(
            REFERENCE_WINDOW_NAME, title="MtoU 场景参考",
            closeCommand=self._on_closed, sizeable=False, width=WINDOW_WIDTH,
            resizeToFitChildren=True)
        cmds.columnLayout(adjustableColumn=True, rowSpacing=ROW_SPACING,
                          columnAttach=("both", CARD_MARGIN))
        cmds.text(label="", height=1)

        offer_card = cmds.frameLayout(
            label="参考交接", collapsable=False,
            marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN - 3)
        cmds.columnLayout(adjustableColumn=False, rowSpacing=6)
        self._transfer_text = cmds.text(label="交接：暂无", align="left")
        self._scope_text = cmds.text(label="范围：—", align="left")
        self._digest_text = cmds.text(label="摘要：—", align="left")
        self._counts_text = cmds.text(label="计数：—", align="left")
        self._space_text = cmds.text(label="空间版本：—", align="left")
        cmds.setParent("..")
        cmds.setParent("..")

        budget_card = cmds.frameLayout(
            label="冻结上限", collapsable=False,
            marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN - 3)
        cmds.rowLayout(numberOfColumns=1, columnWidth1=WINDOW_CONTENT)
        self._budget_text = cmds.text(label="—", align="left", wordWrap=True, height=54)
        cmds.setParent("..")
        cmds.setParent("..")

        report_card = cmds.frameLayout(
            label="导入结果", collapsable=False,
            marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN - 3)
        cmds.rowLayout(numberOfColumns=1, columnWidth1=WINDOW_CONTENT)
        self._report_text = cmds.text(label="还没有导入记录。", align="left",
                                      wordWrap=True, height=96)
        cmds.setParent("..")
        cmds.setParent("..")

        cmds.formLayout(width=WINDOW_CONTENT)
        self._import_button = cmds.button(
            label="导入/更新参考", height=34, enable=False,
            command=lambda *_: self._controller.import_reference())
        self._remove_button = cmds.button(
            label="移除参考", height=34,
            command=lambda *_: self._controller.remove_reference())
        self._recover_button = cmds.button(
            label="恢复上次更新", height=34,
            command=lambda *_: self._controller.recover_reference())
        grid_row([self._import_button, self._remove_button, self._recover_button],
                 height=34 + GRID_GAP, top=GRID_GAP)
        cmds.formLayout(width=WINDOW_CONTENT)
        self._close_button = cmds.button(label="关闭窗口", height=34,
                                         command=lambda *_: self.close())
        grid_row([self._close_button], height=34 + GRID_GAP, top=GRID_GAP)
        cmds.setParent("..")
        cmds.setParent("..")

        cmds.showWindow(REFERENCE_WINDOW_NAME)
        self._card_decorations, self._styled_controls = apply_panel_theme(
            REFERENCE_WINDOW_NAME,
            (offer_card, budget_card, report_card),
            {self._import_button: "primary"})
        self.refresh()

    # ------------------------------------------------------------- actions
    def refresh(self):
        """Render the controller's current handoff, budgets and report."""
        offer = self._controller.reference_offer()
        if offer:
            set_text(self._transfer_text, "交接：{0}".format(
                offer.get("transfer_id") or "—"))
            set_text(self._scope_text, "范围：{0}".format(_scope_summary(offer)))
            set_text(self._digest_text, "摘要：{0}".format(
                offer.get("manifest_digest") or offer.get("digest") or "—"))
            set_text(self._counts_text, "计数：{0}".format(
                _counts_summary(offer.get("counts"))))
            space_revision = offer.get("space_revision")
            set_text(self._space_text, "空间版本：{0}".format(
                "—" if space_revision is None else space_revision))
        else:
            set_text(self._transfer_text, "交接：暂无")
            set_text(self._scope_text, "范围：—")
            set_text(self._digest_text, "摘要：—")
            set_text(self._counts_text, "计数：—")
            set_text(self._space_text, "空间版本：—")

        budget = self._controller.reference_budget()
        set_text(self._budget_text, "字节上限：{0}（{1:g} MiB）\n"
                 "对象上限：{2}\n三角面上限：{3}".format(
                     _count_label(budget.get("bytes")),
                     float(budget.get("bytes")) / _MIB,
                     _count_label(budget.get("objects")),
                     _count_label(budget.get("triangles"))))
        set_text(self._report_text, "\n".join(
            _report_lines(self._controller.reference_report())))

        controls = self._controller.reference_window_controls()
        for button, control in ((self._import_button, controls.get("import")),
                                (self._remove_button, controls.get("remove")),
                                (self._recover_button, controls.get("recover"))):
            if not isinstance(control, (list, tuple)) or len(control) != 2:
                continue
            set_enabled(button, control[0])
            set_tooltip(button, control[1])

    def show(self):
        cmds = mayaapi.cmds
        if cmds is not None:
            cmds.showWindow(REFERENCE_WINDOW_NAME)

    def close(self):
        cmds = mayaapi.cmds
        if cmds is not None and cmds.window(REFERENCE_WINDOW_NAME, exists=True):
            cmds.deleteUI(REFERENCE_WINDOW_NAME)

    # ------------------------------------------------------------- internals
    def _on_closed(self, *unused):
        del unused
        # Closing this window must never leave, import or remove anything.
        self._controller.close_reference_window()
