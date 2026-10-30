"""场景参考 detail window: one Unreal handoff offer and its import outcome.

The window owns no state: the offered handoff, the last report and even the
button enablement come from controller getters, and every button sends a
controller action. It shows where the reference comes from, its size, whether
it can be imported and the last result in one sentence; the handoff identity,
digest, space revision and frozen budgets are in 诊断详情.
"""

import time

from .. import mayaapi
from .layout import (
    apply_panel_theme,
    elide_text,
    grid_row,
    release_panel_theme,
    set_enabled,
    set_text,
    set_tooltip,
)
from .style import CARD_MARGIN, GRID_GAP, ROW_SPACING

REFERENCE_WINDOW_NAME = "MtoULiveLinkReferenceWindow"

WINDOW_WIDTH = 520

WINDOW_CONTENT = WINDOW_WIDTH - 2 * CARD_MARGIN

# A card's body sits inside its margins and Maya's one-pixel frame; a wider
# label widens the whole window past WINDOW_WIDTH.
CARD_BODY = WINDOW_CONTENT - 2 * (CARD_MARGIN + 1)

# The longest line (a CJK character counts two) a card label shows; a longer
# level list ends in an ellipsis and the tooltip lists every level.
LINE_UNITS = 68


def _package_name(path):
    """The last segment of an Unreal package path: what the user calls the level."""
    text = str(path or "").rstrip("/")
    return text.rsplit("/", 1)[-1].split(".", 1)[0] if text else "—"


def reference_scope_summary(offer):
    """The handoff scope as one diagnostic line."""
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


def reference_counts_summary(counts):
    """Exact object, triangle and byte counts as one diagnostic line."""
    if not isinstance(counts, dict) or not counts:
        return "—"
    parts = []
    for key, label in (("objects", "对象"), ("triangles", "三角面"), ("bytes", "字节")):
        if counts.get(key) is not None:
            parts.append("{0} {1}".format(label, counts.get(key)))
    return "｜".join(parts) if parts else "—"


def reference_source_lines(offer):
    """Return ``(level line, sublevel line, tooltip)`` for the offered scope."""
    scope = offer.get("scope") if isinstance(offer, dict) else None
    if not isinstance(scope, dict) or not scope:
        return "来源关卡：—", "子关卡：—", ""
    level = scope.get("level")
    sublevels = [str(name) for name in scope.get("sublevels") or ()]
    unloaded = [str(name) for name in scope.get("unloaded_sublevels") or ()]
    level_line = "来源关卡：{0}".format(_package_name(level))
    if sublevels:
        sublevel_line = "子关卡：{0}".format("、".join(
            _package_name(name) for name in sublevels))
    else:
        sublevel_line = "子关卡：无"
    if unloaded:
        sublevel_line += "（{0} 个未加载，未包含）".format(len(unloaded))
    tooltip = "\n".join([str(level or "—")] + sublevels)
    return (elide_text(level_line, LINE_UNITS),
            elide_text(sublevel_line, LINE_UNITS), tooltip)


def _approximate(value, unit_label):
    value = int(value)
    for scale, name in ((100000000, "亿"), (10000, "万")):
        if value >= scale:
            amount = value / float(scale)
            amount = round(amount) if amount >= 100 else round(amount, 1)
            return "约 {0:g} {1}{2}".format(amount, name, unit_label)
    return "{0} {1}".format(value, unit_label)


def reference_scale_text(counts):
    """规模 in the user's terms, for example “152 个对象，约 120.3 万三角面”."""
    if not isinstance(counts, dict) or counts.get("objects") is None:
        return "规模：—"
    text = "规模：{0} 个对象".format(int(counts["objects"]))
    if counts.get("triangles") is not None:
        text += "，{0}".format(_approximate(counts["triangles"], "三角面"))
    return text


def _clock(seconds):
    if seconds is None:
        return ""
    return time.strftime("%H:%M", time.localtime(seconds))


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


def reference_report_lines(outcome):
    """Every field of the last import, removal or recovery, for 诊断详情."""
    action = outcome.get("action")
    report = outcome.get("report")
    if not isinstance(report, dict) or not report:
        return ["还没有导入记录。"]
    if action == "import":
        lines = ["导入：{0}".format(
            "已接受" if report.get("accepted") else "已拒绝")]
        if report.get("code"):
            lines.append("代码：{0}".format(report.get("code")))
        detail = report.get("detail")
        if detail:
            lines.append("说明：{0}".format(detail))
        lines.append("丢弃节点：{0}".format("是" if report.get("discarded") else "否"))
        lines.append("回滚：{0}".format("是" if report.get("rolled_back") else "否"))
        lines.append("残留：{0}".format(_residual_text(report.get("residual"))))
        counts = report.get("counts")
        if isinstance(counts, dict):
            lines.append("计数：{0}".format(reference_counts_summary(counts)))
        container = report.get("container")
        if isinstance(container, dict) and container.get("name"):
            lines.append("容器：{0}".format(container["name"]))
        anchor = report.get("anchor")
        if isinstance(anchor, dict) and anchor.get("source"):
            lines.append("锚点：{0}".format(anchor.get("source")))
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


def reference_result_text(outcome):
    """The report card: one sentence, plus the failure detail only on failure."""
    action = outcome.get("action")
    report = outcome.get("report")
    if not isinstance(report, dict) or not report:
        return "还没有导入记录。"
    if action == "import":
        if report.get("accepted"):
            counts = report.get("counts") or {}
            text = "已导入 {0} 个对象。".format(counts.get("objects") or 0)
            anchor = report.get("anchor")
            if isinstance(anchor, dict) and anchor.get("source") == "absent":
                text += "交接清单未带锚点，参考放在 Maya 原点；请在 UE 重新导出后再次导入。"
            return text
        lines = ["导入失败：{0}".format(report.get("detail") or "未知原因")]
        if report.get("rolled_back"):
            lines.append("已回滚，原参考保留。")
        if report.get("discarded"):
            lines.append("已丢弃导入的节点。")
        residual = _residual_text(report.get("residual"))
        if residual != "无":
            lines.append("残留：{0}。点击“恢复上次更新”清理后重试。".format(residual))
        return "\n".join(lines)
    if action == "remove":
        if not report.get("removed"):
            text = "场景中没有需要移除的参考。"
        else:
            text = "已移除参考（{0} 个节点）。".format(report.get("nodes_removed") or 0)
        residual = _residual_text(report.get("residual"))
        if residual != "无":
            text += "\n残留：{0}。请手动删除这些节点。".format(residual)
        return text
    if action == "recover":
        if report.get("partial_cleanups") or report.get("skipped_namespaces"):
            return ("上次更新已恢复，但有部分内容未能清理；"
                    "打开主面板的“诊断详情”查看。")
        if report.get("restored_previous_reference"):
            return "已恢复上一次的参考。"
        return "已完成上次更新的恢复。"
    return "还没有导入记录。"


class ReferenceWindow(object):
    """Shows the offered handoff's source, size, state and the last result."""

    def __init__(self, controller):
        self._controller = controller
        self._level_text = None
        self._sublevel_text = None
        self._scale_text = None
        self._state_text = None
        self._report_text = None
        self._import_button = None
        self._remove_button = None
        self._recover_button = None
        self._close_button = None
        self._card_decorations = []
        self._styled_controls = {}
        self._closed = False
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
        self._level_text = cmds.text(label="来源关卡：—", align="left", width=CARD_BODY)
        self._sublevel_text = cmds.text(label="子关卡：—", align="left", width=CARD_BODY)
        self._scale_text = cmds.text(label="规模：—", align="left", width=CARD_BODY)
        self._state_text = cmds.text(label="状态：暂无交接", align="left", width=CARD_BODY)
        cmds.setParent("..")
        cmds.setParent("..")

        report_card = cmds.frameLayout(
            label="导入结果", collapsable=False,
            marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN - 3)
        cmds.rowLayout(numberOfColumns=1, columnWidth1=WINDOW_CONTENT)
        self._report_text = cmds.text(label="还没有导入记录。", align="left",
                                      wordWrap=True, height=72, width=CARD_BODY)
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
        self._close_button = cmds.button(
            label="关闭窗口", height=34,
            command=lambda *_: self._controller.close_reference_window())
        grid_row([self._close_button], height=34 + GRID_GAP, top=GRID_GAP)
        cmds.setParent("..")
        cmds.setParent("..")

        cmds.showWindow(REFERENCE_WINDOW_NAME)
        self._card_decorations, self._styled_controls = apply_panel_theme(
            REFERENCE_WINDOW_NAME,
            (offer_card, report_card),
            {self._import_button: "primary"})
        self.refresh()

    # ------------------------------------------------------------- actions
    def refresh(self):
        """Render the controller's current handoff, state and last result."""
        if self._closed:
            return
        offer = self._controller.reference_offer()
        if offer:
            level, sublevels, tooltip = reference_source_lines(offer)
            set_text(self._level_text, level, tooltip=tooltip)
            set_text(self._sublevel_text, sublevels, tooltip=tooltip)
            set_text(self._scale_text, reference_scale_text(offer.get("counts")))
        else:
            set_text(self._level_text, "来源关卡：—")
            set_text(self._sublevel_text, "子关卡：—")
            set_text(self._scale_text, "规模：—")
        state, state_tooltip = self._controller.reference_state()
        set_text(self._state_text, "状态：{0}".format(state), tooltip=state_tooltip)
        set_text(self._report_text, reference_result_text(
            self._controller.reference_report()))

        controls = self._controller.reference_window_controls()
        for button, control in ((self._import_button, controls.get("import")),
                                (self._remove_button, controls.get("remove")),
                                (self._recover_button, controls.get("recover"))):
            if not isinstance(control, (list, tuple)) or len(control) != 2:
                continue
            set_enabled(button, control[0])
            set_tooltip(button, control[1])

    def exists(self):
        """True while this instance's Maya window is still open."""
        cmds = mayaapi.cmds
        return bool(not self._closed and cmds is not None
                    and cmds.window(REFERENCE_WINDOW_NAME, exists=True))

    def show(self):
        cmds = mayaapi.cmds
        if cmds is not None:
            cmds.showWindow(REFERENCE_WINDOW_NAME)

    def close(self):
        """Delete this window; Maya then sends its close notification."""
        if self._closed:
            return
        decorations = self._card_decorations
        self._forget_widgets()
        release_panel_theme(decorations)
        cmds = mayaapi.cmds
        if cmds is not None and cmds.window(REFERENCE_WINDOW_NAME, exists=True):
            cmds.deleteUI(REFERENCE_WINDOW_NAME)

    # ------------------------------------------------------------- internals
    def _forget_widgets(self):
        self._closed = True
        self._card_decorations = []
        self._styled_controls = {}

    def _on_closed(self, *unused):
        """Maya is closing this window; its close event is still running.

        Deleting the window again from here makes Maya read the destroyed
        window once the event returns, so this only tells the controller.
        Closing this window must never leave, import or remove anything.
        """
        del unused
        self._forget_widgets()
        self._controller.reference_window_closed(self)


def import_time_label(seconds):
    """The clock time an import finished, as the state line shows it."""
    return _clock(seconds)
