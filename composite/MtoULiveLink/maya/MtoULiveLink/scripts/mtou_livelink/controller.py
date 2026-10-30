"""Product controller: panel, session and scene state authority."""

import contextlib
import time

from . import mayaapi
from .cache import CachedPlayback, CachedPlaybackError
from .config import (
    CACHED_MODE,
    CHARACTER_OBJECT_ID,
    CONTROL_MAYA,
    CONTROL_UNREAL,
    DEFAULT_PLAYBACK_CAP,
    LINKAGE_MODE,
    MAX_REFERENCE_BYTES,
    MAX_REFERENCE_OBJECTS,
    MAX_REFERENCE_TRIANGLES,
    PLAYBACK_CAP_CHOICES,
    REALTIME_MODE,
    REFERENCE_CONTAINER,
    SECONDARY_OBJECT_ID,
    WORKFLOW_ANIMATION,
    WORKFLOW_MODEL,
    WORKFLOWS,
)
from .diagnostics import DIAGNOSTICS, make_diagnostic
from .linkage import LinkageError, LinkageSession, is_scheduling_conflict
from .mayaapi import cmds, om, omui
from .protocol import (
    exact_int,
    make_mode_message,
    make_reference_removed_message,
    make_reference_result_message,
    validate_reply,
)
from .scene import CharacterScene, CharacterSceneError, long_transform, require_maya, selected_root
from .session import StreamingSession, StreamingSessionError, bind_conflict_status
from .space import format_fps, frames_per_second, validate_frame_rate
from .timeline import load_playback_cap, save_playback_cap
from .ui.layout import (
    apply_panel_theme,
    elide_text,
    grid_row,
    release_panel_theme,
    text_units,
)
from .ui.objects_window import OBJECTS_WINDOW_NAME, ObjectsWindow
from .ui.reference_window import (
    REFERENCE_WINDOW_NAME,
    ReferenceWindow,
    import_time_label,
    reference_counts_summary,
    reference_report_lines,
    reference_scope_summary,
)
from .ui.style import (
    BUTTON_HEIGHT,
    CARD_CONTENT,
    CARD_GAP,
    CARD_HALF,
    CARD_MARGIN,
    GRID_GAP,
    LIGHT_OFF_BACKGROUND,
    LIGHT_ON_BACKGROUND,
    PANEL_MARGIN,
    PANEL_WIDTH,
    PRIMARY_BACKGROUND,
    ROW_SPACING,
    TAB_HEIGHT,
    TAB_OFF_BACKGROUND,
    TAB_ON_BACKGROUND,
    TOGGLE_OFF_BACKGROUND,
    TOGGLE_ON_BACKGROUND,
)


class Controller(object):
    def __init__(self):
        self._session = None
        self._scene = None
        self._pending_root = None
        self._capture_args = None
        self._script_jobs = []
        self._outfit_change_was_connected = False
        self._pending_stop_status = None
        self._last_diagnostic = make_diagnostic("INTERNAL_ERROR", "暂无诊断信息")
        self._last_warning = {"missing_in_unreal": [], "missing_in_maya": [],
                              "bone_name_remaps": [],
                              "has_warning": False}
        self._light = None
        self._root_text = None
        self._outfit_text = None
        self._fps_text = None
        self._playback_cap = DEFAULT_PLAYBACK_CAP
        self._playback_cap_menu = None
        self._mode = REALTIME_MODE
        self._realtime_mode_button = None
        self._cached_mode_button = None
        self._capture_button = None
        self._upload_button = None
        self._cancel_capture_button = None
        self._range_row = None
        self._custom_range_checkbox = None
        self._range_start_field = None
        self._range_end_field = None
        self._cache_text = None
        self._cached_playback = None
        self._cached_view = None
        self._cached_retention = None
        self._maya_exit_callback = None
        # Before-open/new callbacks: linkage restores the scene it captured
        # while that scene is still the open one.
        self._scene_replace_callbacks = []
        self._workflow = WORKFLOW_ANIMATION
        self._blendshapes_enabled = True
        self._animation_workflow_button = None
        self._model_workflow_button = None
        self._bs_checkbox = None
        self._bone_text = None
        self._curve_text = None
        self._status_text = None
        self._cache_row = None
        self._connect_button = None
        self._disconnect_button = None
        self._shown_warning_signatures = set()
        self._warning_checkbox = None
        self._duplicate_button = None
        self._styled_controls = {}
        self._card_decorations = []
        self._resize_scheduled = False
        self._linkage = None
        self._linkage_pump = None
        self._linkage_pump_generation = 0
        self._reference_busy = 0
        self._linkage_last_recovery = []
        self._linkage_event = None
        self._reported_linkage_failure = None
        # Last mode revision of this connection; Unreal keeps counting across
        # entries, so every new linkage continues from it.
        self._linkage_revision = 0
        # Cached Playback's reply queue once it owned the connection; replies
        # linkage does not handle keep reaching it as before.
        self._cached_reply_sink = None
        self._reference_offer = None
        self._reference_report = {"action": None, "report": None}
        self._reference_window = None
        self._enter_linkage_button = None
        self._leave_linkage_button = None
        self._end_linkage_button = None
        self._reference_window_button = None
        self._link_status_text = None
        # The independent object beside the primary character; it joins the
        # set the next connection negotiates. The pairing window shows it.
        self._secondary_scene = None
        self._objects_window = None
        self._objects_window_button = None
        self._object_set_detail = ""

    @staticmethod
    def _grid_row(controls, spans=None, height=BUTTON_HEIGHT, top=0):
        """Attach controls of the current formLayout to equal grid columns."""
        return grid_row(controls, spans, height, top)

    def build_ui(self):
        cmds.window(WINDOW_NAME, title="MtoU Live Link", closeCommand=self.close,
                    sizeable=False, width=PANEL_WIDTH + 2 * PANEL_MARGIN,
                    resizeToFitChildren=True)
        cmds.columnLayout(adjustableColumn=True, rowSpacing=ROW_SPACING,
                          columnAttach=("both", PANEL_MARGIN))
        cmds.text(label="", height=1)

        # Card 1 · workflow tabs.
        workflow_card = cmds.frameLayout(labelVisible=False, collapsable=False,
                                         marginWidth=6, marginHeight=6)
        cmds.rowLayout(numberOfColumns=2, columnWidth2=(CARD_HALF, CARD_HALF + CARD_GAP),
                       columnAttach2=("both", "both"))
        self._animation_workflow_button = cmds.button(
            label="动画", height=TAB_HEIGHT, backgroundColor=TAB_ON_BACKGROUND,
            annotation="动画工作流：实时预览或缓存播放角色动画。",
            command=lambda *_: self._on_workflow_changed(WORKFLOW_ANIMATION))
        self._model_workflow_button = cmds.button(
            label="模型", height=TAB_HEIGHT, backgroundColor=TAB_OFF_BACKGROUND,
            annotation="模型工作流：在 UE 用 Generated Preview 检查服装。",
            command=lambda *_: self._on_workflow_changed(WORKFLOW_MODEL))
        cmds.setParent("..")
        cmds.setParent("..")

        # Card 2 · connect.
        connection_card = cmds.frameLayout(label="连接控制", collapsable=False,
                                           marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN)
        cmds.formLayout(width=CARD_CONTENT)
        set_role = cmds.button(label="设置角色", height=BUTTON_HEIGHT,
                               annotation="先在 Maya 中选择变形根骨骼，再设置角色。",
                               command=lambda *_: self.set_role())
        self._connect_button = cmds.button(
            label="连接", height=BUTTON_HEIGHT, backgroundColor=PRIMARY_BACKGROUND,
            annotation="把当前角色连接到 Unreal Binding Actor。",
            command=lambda *_: self.connect())
        self._disconnect_button = cmds.button(
            label="断开", height=BUTTON_HEIGHT, command=lambda *_: self.disconnect())
        self._light = cmds.text(label="●  未连接", align="center", height=BUTTON_HEIGHT,
                                font="boldLabelFont", backgroundColor=LIGHT_OFF_BACKGROUND)
        self._grid_row([set_role, self._connect_button, self._disconnect_button, self._light])
        cmds.setParent("..")

        # Card 3 · scene info, two label columns.
        scene_card = cmds.frameLayout(label="场景信息", collapsable=False,
                                      marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN + 2)
        cmds.rowLayout(numberOfColumns=2, columnWidth2=(CARD_HALF, CARD_HALF),
                       columnAttach2=("left", "left"))
        cmds.columnLayout(adjustableColumn=False, rowSpacing=6)
        self._root_text = cmds.text(label="角色根骨骼：—", align="left",
                                    width=SCENE_TEXT_WIDTH)
        self._outfit_text = cmds.text(label="当前衣服：—", align="left",
                                      width=SCENE_TEXT_WIDTH)
        self._fps_text = cmds.text(label="场景帧率：—", align="left",
                                   width=SCENE_TEXT_WIDTH)
        cmds.setParent("..")
        cmds.columnLayout(adjustableColumn=False, rowSpacing=6,
                          columnAttach=("left", CACHE_TEXT_INDENT))
        self._bone_text = cmds.text(label="骨骼数：0", align="left",
                                    width=CACHE_TEXT_WIDTH)
        self._curve_text = cmds.text(label="BlendShape 数：0", align="left",
                                     width=CACHE_TEXT_WIDTH)
        self._cache_text = cmds.text(label="缓存：无", annotation="缓存：无", align="left",
                                     height=18, width=CACHE_TEXT_WIDTH)
        cmds.setParent("..")
        cmds.setParent("..")
        cmds.setParent("..")

        # Card 4 · preview modes, transfer cap, and the cache action row.
        preview_card = cmds.frameLayout(label="预览与播放", collapsable=False,
                                        marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN)
        cmds.columnLayout(adjustableColumn=True, rowSpacing=0)
        cmds.formLayout(width=CARD_CONTENT)
        self._realtime_mode_button = cmds.button(
            label="实时预览", height=BUTTON_HEIGHT, backgroundColor=TOGGLE_ON_BACKGROUND,
            command=lambda *_: self._on_mode_changed(REALTIME_MODE))
        self._cached_mode_button = cmds.button(
            label="缓存播放", height=BUTTON_HEIGHT, backgroundColor=TOGGLE_OFF_BACKGROUND,
            command=lambda *_: self._on_mode_changed(CACHED_MODE))
        self._bs_checkbox = cmds.checkBox(
            label="传递 BS", value=True, height=BUTTON_HEIGHT,
            annotation="连接时把 Maya BlendShape 值一起传给 Unreal。",
            changeCommand=lambda *_: self._on_blendshapes_toggled())
        self._playback_cap = load_playback_cap()
        self._playback_cap_menu = cmds.optionMenu(
            label="上限", height=BUTTON_HEIGHT, changeCommand=self._on_playback_cap_changed)
        for choice in PLAYBACK_CAP_CHOICES:
            cmds.menuItem(label=choice)
        cmds.optionMenu(self._playback_cap_menu, edit=True, value=self._playback_cap)
        # The mode buttons and the BS switch share the first two columns; only
        # one of them is managed per workflow.
        self._grid_row([self._realtime_mode_button, self._cached_mode_button,
                        self._playback_cap_menu])
        self._range_row = cmds.rowLayout(
            numberOfColumns=5, columnWidth5=(156, 38, 72, 38, 72),
            columnAttach5=("left", "left", "left", "left", "left"))
        self._custom_range_checkbox = cmds.checkBox(
            label="自定义捕获范围", value=False,
            changeCommand=lambda *_: self._update_custom_range_controls())
        cmds.text(label="开始")
        self._range_start_field = cmds.intField(value=1, width=72)
        cmds.text(label="结束")
        self._range_end_field = cmds.intField(value=24, width=72)
        cmds.setParent("..")
        self._cache_row = cmds.formLayout(width=CARD_CONTENT)
        self._capture_button = cmds.button(
            label="捕获并上传", height=BUTTON_HEIGHT, enable=False,
            command=lambda *_: self._capture_cached_playback())
        self._upload_button = cmds.button(
            label="上传保留缓存", height=BUTTON_HEIGHT, enable=False,
            command=lambda *_: self._upload_retained_cache())
        self._cancel_capture_button = cmds.button(
            label="取消捕获", height=BUTTON_HEIGHT, enable=False,
            command=lambda *_: self._cancel_cached_capture())
        self._grid_row([self._capture_button, self._upload_button,
                        self._cancel_capture_button],
                       height=BUTTON_HEIGHT + GRID_GAP, top=GRID_GAP)
        cmds.setParent("..")
        cmds.setParent("..")

        # Card 5 · single-character linkage: one status line, enter and leave,
        # and the scene-reference window.
        linkage_card = cmds.frameLayout(label="镜头联动", collapsable=False,
                                        marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN)
        cmds.columnLayout(adjustableColumn=True, rowSpacing=0)
        cmds.rowLayout(numberOfColumns=1, columnWidth1=CARD_CONTENT)
        self._link_status_text = cmds.text(label="未进入联动", align="left",
                                           width=LINK_STATUS_WIDTH)
        cmds.setParent("..")
        cmds.formLayout(width=CARD_CONTENT)
        self._enter_linkage_button = cmds.button(
            label="进入联动", height=BUTTON_HEIGHT, backgroundColor=PRIMARY_BACKGROUND,
            annotation="进入镜头联动：Unreal 控制序列时间和相机，Maya 跟随显示角色姿势。",
            command=lambda *_: self.enter_linkage())
        self._leave_linkage_button = cmds.button(
            label="返回实时", height=BUTTON_HEIGHT, enable=False,
            command=lambda *_: self.leave_linkage("realtime"))
        self._end_linkage_button = cmds.button(
            label="结束预览", height=BUTTON_HEIGHT, enable=False,
            command=lambda *_: self.leave_linkage("end"))
        self._grid_row([self._enter_linkage_button, self._leave_linkage_button,
                        self._end_linkage_button],
                       height=BUTTON_HEIGHT + GRID_GAP, top=GRID_GAP)
        cmds.formLayout(width=CARD_CONTENT)
        self._objects_window_button = cmds.button(
            label="打开对象配对窗口", height=BUTTON_HEIGHT,
            annotation="设置或移除与主角色一起预览的副对象。",
            command=lambda *_: self.open_objects_window())
        self._reference_window_button = cmds.button(
            label="打开场景参考窗口", height=BUTTON_HEIGHT,
            annotation="查看、导入、移除或恢复 UE 场景参考。",
            command=lambda *_: self.open_reference_window())
        self._grid_row([self._objects_window_button, self._reference_window_button],
                       height=BUTTON_HEIGHT + GRID_GAP, top=GRID_GAP)
        cmds.setParent("..")
        cmds.setParent("..")

        # Card 6 · advanced tools.
        tools_card = cmds.frameLayout(label="工具", collapsable=False,
                                      marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN - 3)
        cmds.formLayout(width=CARD_CONTENT)
        display = cmds.button(label="选择 Display 控制器", height=BUTTON_HEIGHT,
                              annotation="检测到多个服装属性时，选中 Display 控制器后点击。",
                              command=lambda *_: self.set_display_controller())
        self._duplicate_button = cmds.button(
            label="选中重名骨骼（0）", height=BUTTON_HEIGHT, enable=False,
            annotation="重名会被 UE 自动映射；点击可在场景中选中所有冲突骨骼。",
            command=lambda *_: self.select_duplicate_bones())
        self._grid_row([display, self._duplicate_button])
        cmds.setParent("..")

        # Card 6 · diagnostics.
        diagnostics_card = cmds.frameLayout(label="诊断", collapsable=False,
                                            marginWidth=CARD_MARGIN, marginHeight=CARD_MARGIN - 3)
        cmds.rowLayout(numberOfColumns=2, adjustableColumn=1,
                       columnWidth2=(CARD_CONTENT - 110, 110),
                       columnAttach2=("both", "both"), columnOffset2=(0, 4))
        self._status_text = cmds.text(label="未设置角色：选择根骨骼后点击“设置角色”",
                                      align="left", wordWrap=True)
        cmds.button(label="诊断详情", height=BUTTON_HEIGHT,
                    command=lambda *_: self.show_diagnostics())
        cmds.setParent("..")
        cmds.setParent("..")

        cmds.columnLayout(adjustableColumn=True, rowSpacing=4)
        cmds.separator(height=1, style="single")
        cmds.rowLayout(numberOfColumns=1, columnWidth1=CARD_CONTENT)
        self._warning_checkbox = cmds.checkBox(
            label="连接成功后弹出差异警告", value=True)
        cmds.setParent("..")
        cmds.setParent("..")
        self._refresh_fps()
        self._update_custom_range_controls()
        self._update_mode_controls()
        self._update_workflow_controls()
        self._update_linkage_status()
        self._script_jobs.append(cmds.scriptJob(
            event=["timeUnitChanged", self._on_time_unit_changed], parent=WINDOW_NAME))
        self._script_jobs.append(cmds.scriptJob(
            event=["SceneOpened", self._on_scene_change], parent=WINDOW_NAME))
        self._script_jobs.append(cmds.scriptJob(
            event=["NewSceneOpened", self._on_scene_change], parent=WINDOW_NAME))
        if om is not None:
            try:
                self._maya_exit_callback = om.MSceneMessage.addCallback(
                    om.MSceneMessage.kMayaExiting, self._on_maya_exiting)
            except (AttributeError, RuntimeError, TypeError):
                self._maya_exit_callback = None
            for message in ("kBeforeNew", "kBeforeOpen"):
                try:
                    self._scene_replace_callbacks.append(om.MSceneMessage.addCallback(
                        getattr(om.MSceneMessage, message),
                        self._on_before_scene_replaced))
                except (AttributeError, RuntimeError, TypeError):
                    pass
        cmds.showWindow(WINDOW_NAME)
        self._apply_panel_theme(
            (workflow_card, connection_card, scene_card, preview_card,
             linkage_card, tools_card, diagnostics_card),
            {self._animation_workflow_button: "tabOn",
             self._model_workflow_button: "tabOff",
             self._connect_button: "primary",
             self._realtime_mode_button: "realtime",
             self._cached_mode_button: "neutral",
             self._light: "disconnected",
             self._capture_button: "primary",
             self._enter_linkage_button: "primary"},
            divider_card=scene_card)


    def _apply_panel_theme(self, cards, roles, divider_card=None):
        """Style only this Maya window's Qt widgets; keep cmds as state owner."""
        self._card_decorations, self._styled_controls = apply_panel_theme(
            WINDOW_NAME, cards, roles, divider_card=divider_card)

    def _set_style_role(self, control, role):
        widget = self._styled_controls.get(control)
        if widget is not None and widget.property("mtouRole") != role:
            widget.setProperty("mtouRole", role)
            widget.style().unpolish(widget)
            widget.style().polish(widget)
            widget.update()


    def _control_exists(self, control):
        if not control or cmds is None:
            return False
        try:
            return bool(cmds.control(control, exists=True))
        except (AttributeError, RuntimeError, TypeError):
            return False

    def _set_enabled(self, control, enabled):
        if self._control_exists(control):
            try:
                cmds.control(control, edit=True, enable=bool(enabled))
                if control in (self._connect_button, self._capture_button):
                    cmds.button(control, edit=True, backgroundColor=(
                        PRIMARY_BACKGROUND if enabled else TOGGLE_OFF_BACKGROUND))
            except (AttributeError, RuntimeError, TypeError):
                pass

    def _set_visible(self, control, visible):
        if self._control_exists(control):
            try:
                cmds.control(control, edit=True, visible=bool(visible),
                             manage=bool(visible))
            except (AttributeError, RuntimeError, TypeError):
                pass

    def _set_tooltip(self, control, tooltip):
        if self._control_exists(control):
            try:
                cmds.control(control, edit=True, annotation=tooltip or "")
            except (AttributeError, RuntimeError, TypeError):
                pass

    @staticmethod
    def _disabled_reason(enabled, action):
        if enabled:
            return ""
        reasons = {
            "capture": "先连接 Unreal 并进入缓存播放，再捕获 Maya 播放范围或自定义范围。",
            "upload": "仅在重连后有保留的完整本地缓存时可上传。",
            "cancel": "仅在捕获过程中可取消。",
        }
        return reasons.get(action, "")


    def _warning_signature(self, warning):
        missing_unreal = tuple(sorted(warning.get("missing_in_unreal") or ()))
        missing_maya = tuple(sorted(warning.get("missing_in_maya") or ()))
        remaps = tuple(sorted(warning.get("bone_name_remaps") or ()))
        return (missing_unreal, missing_maya, remaps)

    def _session_ready(self):
        session = self._session
        if session is None:
            return False
        ready = getattr(session, "is_ready", None)
        if callable(ready):
            try:
                return bool(ready())
            except (AttributeError, RuntimeError, TypeError):
                return False
        if ready is not None:
            return bool(ready)
        return False

    def _apply_toggle_background(self, control, selected):
        if not self._control_exists(control):
            return
        try:
            cmds.button(control, edit=True, backgroundColor=(
                TOGGLE_ON_BACKGROUND if selected else TOGGLE_OFF_BACKGROUND))
        except (AttributeError, RuntimeError, TypeError):
            pass
        self._set_style_role(control, "realtime" if selected else "neutral")

    def _update_mode_selection(self):
        self._apply_toggle_background(
            self._realtime_mode_button, self._mode == REALTIME_MODE)
        self._apply_toggle_background(
            self._cached_mode_button, self._mode == CACHED_MODE)

    def _update_mode_controls(self):
        realtime = self._mode == REALTIME_MODE
        view = self._cached_view
        can_capture = bool(view and view.can_capture)
        can_upload = bool(view and view.can_upload)
        can_cancel = bool(view and view.can_cancel)
        can_leave = bool(view and view.can_leave)
        connected = self._session_ready()
        linking = self._linkage_active()
        two_objects = self._session_has_secondary()
        self._set_enabled(self._playback_cap_menu, realtime and not can_cancel)
        self._set_enabled(self._capture_button, not realtime and can_capture)
        self._set_enabled(self._upload_button, not realtime and can_upload)
        self._set_enabled(self._cancel_capture_button, not realtime and can_cancel)
        self._set_enabled(self._realtime_mode_button, realtime or can_leave)
        self._set_enabled(self._cached_mode_button,
                          connected and not can_cancel and not linking and not two_objects)
        self._set_enabled(self._connect_button, not connected)
        self._set_enabled(self._disconnect_button, connected or self._scene is not None)
        self._set_tooltip(
            self._capture_button, self._disabled_reason(can_capture, "capture"))
        self._set_tooltip(
            self._upload_button, self._disabled_reason(can_upload, "upload"))
        self._set_tooltip(
            self._cancel_capture_button, self._disabled_reason(can_cancel, "cancel"))
        self._set_tooltip(
            self._cached_mode_button,
            "先设置角色并连接 Unreal。" if not connected else
            ("缓存播放只支持单个角色：先在对象配对窗口中移除副对象。" if two_objects else
             "镜头联动进行中：请先“返回实时”或“结束预览”。" if linking else
             ("等待捕获完成，或取消捕获。" if can_cancel else "捕获并上传范围，在 UE 开始播放。")))
        self._set_tooltip(
            self._realtime_mode_button,
            "在 Maya 摆姿或播放，实时更新 UE。" if realtime or can_leave
            else "等待当前缓存操作完成后返回实时预览。")
        self._set_tooltip(
            self._playback_cap_menu,
            "限制实时预览的传输帧率，不改变 Maya 场景帧率。"
            if realtime and not can_cancel else "返回实时预览后可调整传输上限。")
        self._update_mode_selection()
        self._update_custom_range_controls()
        self._update_context_layout()
        self._update_linkage_controls()

    def _update_custom_range_controls(self):
        enabled = False
        if self._control_exists(self._custom_range_checkbox):
            enabled = bool(cmds.checkBox(
                self._custom_range_checkbox, query=True, value=True))
        editable = self._mode == CACHED_MODE and not (
            self._cached_view and self._cached_view.state == CachedPlayback.CAPTURING)
        self._set_enabled(self._custom_range_checkbox, editable)
        for field in (self._range_start_field, self._range_end_field):
            self._set_enabled(field, editable and enabled)

    def _selected_capture_range(self):
        if not self._control_exists(self._custom_range_checkbox):
            return None
        if not cmds.checkBox(self._custom_range_checkbox, query=True, value=True):
            return None
        return (cmds.intField(self._range_start_field, query=True, value=True),
                cmds.intField(self._range_end_field, query=True, value=True))

    def _update_workflow_controls(self):
        animation = self._workflow == WORKFLOW_ANIMATION
        self._apply_tab_background(self._animation_workflow_button, animation)
        self._apply_tab_background(self._model_workflow_button, not animation)
        for control in (self._realtime_mode_button, self._cached_mode_button,
                        self._cache_text):
            self._set_visible(control, animation)
        self._set_visible(self._bs_checkbox, not animation)
        self._update_context_layout()

    def _apply_tab_background(self, control, selected):
        if self._control_exists(control):
            try:
                cmds.button(control, edit=True, backgroundColor=(
                    TAB_ON_BACKGROUND if selected else TAB_OFF_BACKGROUND))
            except (AttributeError, RuntimeError, TypeError):
                pass
            self._set_style_role(control, "tabOn" if selected else "tabOff")

    def _update_context_layout(self):
        visible = self._workflow == WORKFLOW_ANIMATION and self._mode == CACHED_MODE
        for layout in (self._range_row, self._cache_row):
            if layout and cmds is not None and cmds.layout(layout, exists=True):
                if cmds.layout(layout, query=True, manage=True) != visible:
                    cmds.layout(layout, edit=True, visible=visible, manage=visible)
                    if self._card_decorations and not self._resize_scheduled:
                        from PySide2 import QtCore
                        self._resize_scheduled = True
                        QtCore.QTimer.singleShot(0, self._fit_panel_height)

    def _fit_panel_height(self):
        self._resize_scheduled = False
        if not cmds.window(WINDOW_NAME, exists=True):
            return
        from PySide2 import QtCore, QtWidgets
        from shiboken2 import wrapInstance

        window = wrapInstance(int(omui.MQtUtil.findWindow(WINDOW_NAME)),
                              QtWidgets.QWidget)
        checkbox = wrapInstance(int(omui.MQtUtil.findControl(self._warning_checkbox)),
                                QtWidgets.QWidget)
        bottom = checkbox.mapTo(window, QtCore.QPoint(0, 0)).y() + checkbox.height()
        window.resize(window.width(), bottom + PANEL_MARGIN)

    def _on_workflow_changed(self, workflow):
        if workflow == self._workflow or workflow not in WORKFLOWS:
            return
        self._workflow = workflow
        # Switching workflows disconnects and clears Animation cached
        # playback while retaining the captured root, Display control, and
        # current outfit. The Maya scene is never edited here.
        self.disconnect(status="已切换到{0}工作流，请重新连接".format(
            "模型" if workflow == WORKFLOW_MODEL else "动画"))
        self._mode = REALTIME_MODE
        self._update_mode_controls()
        self._update_workflow_controls()

    def _on_blendshapes_toggled(self):
        value = True
        if self._bs_checkbox and cmds.control(self._bs_checkbox, exists=True):
            try:
                value = bool(cmds.checkBox(self._bs_checkbox, query=True, value=True))
            except (AttributeError, RuntimeError, TypeError):
                value = True
        if value == self._blendshapes_enabled:
            return
        self._blendshapes_enabled = value
        if self._session is not None:
            self.disconnect(status="传递 BS 已切换，请重新连接")

    def _update_cache_text(self, summary=None):
        label, tooltip = cache_summary_text(summary)
        if self._cache_text and cmds.control(self._cache_text, exists=True):
            cmds.text(self._cache_text, edit=True, label=label, annotation=tooltip)

    def _ensure_cached_playback(self):
        if self._workflow != WORKFLOW_ANIMATION or self._session is None \
                or self._scene is None or self._cached_playback is None:
            raise CachedPlaybackError(
                "CACHED_PLAYBACK_NOT_READY",
                "请先完成角色设置并连接 Unreal。")
        return self._cached_playback

    def _on_mode_changed(self, mode):
        if self._linkage_active():
            # Linkage owns the exclusive time-control state; another preview
            # mode would take the session's sampling pause away from the pump.
            self._update_mode_selection()
            self._update_mode_controls()
            self._show_error(make_diagnostic(
                "MODE_CONFLICT",
                "镜头联动进行中，无法切换预览模式。",
                solution="请先“返回实时”或“结束预览”，再切换实时预览与缓存播放。"))
            return
        if mode == CACHED_MODE and self._workflow != WORKFLOW_ANIMATION:
            return
        if mode == CACHED_MODE:
            if self._session_has_secondary():
                self._mode = REALTIME_MODE
                self._update_mode_selection()
                self._update_mode_controls()
                self._show_error(make_diagnostic(
                    "OBJECT_SET_UNSUPPORTED",
                    "缓存播放只支持单个角色。",
                    solution="在“对象配对”窗口中移除副对象后，再进入缓存播放。"))
                return
            if not self._session_ready():
                self._mode = REALTIME_MODE
                self._update_mode_selection()
                self._update_mode_controls()
                self._show_error(make_diagnostic(
                    "CACHED_PLAYBACK_NOT_READY",
                    "请先完成角色设置并连接 Unreal。"))
                return
            try:
                cached = self._ensure_cached_playback()
                cached.enter()
                self._cached_reply_sink = cached.reply_sink()
                self._mode = CACHED_MODE
                self._set_connected(
                    True,
                    "缓存播放：实时采样已暂停，点击“捕获并上传”")
            except CachedPlaybackError as error:
                self._mode = REALTIME_MODE
                self._update_mode_selection()
                self._show_error(make_diagnostic(
                    error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                    error.message, details=error.details))
        else:
            self._mode = REALTIME_MODE
            if self._cached_playback is not None:
                self._cached_playback.leave()
            if self._session is not None:
                self._install_runtime_dispatcher(self._session)
            if self._session_ready():
                self._set_connected(True, "已连接：UE 显示实时姿势")
            else:
                self._set_connected(False, "未连接")
        self._update_mode_controls()

    def _confirm_large_cache(self, estimated_size, total_frames):
        gib = float(estimated_size) / float(1 << 30)
        result = cmds.confirmDialog(
            title="确认大型缓存",
            message="本次捕获约 {0:.2f} GiB（{1} 帧），是否继续？".format(
                gib, total_frames),
            button=["继续", "取消"], defaultButton="继续", cancelButton="取消",
            dismissString="取消")
        return result == "继续"

    def _capture_cached_playback(self):
        if self._mode != CACHED_MODE:
            return
        try:
            cached = self._ensure_cached_playback()
            fps = validate_frame_rate(self._refresh_fps())
            self._set_text(self._status_text, "准备捕获缓存…")
            cached.capture(fps, self._confirm_large_cache,
                           capture_range=self._selected_capture_range())
        except CachedPlaybackError as error:
            diagnostic = make_diagnostic(
                error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                error.message, details=error.details)
            self._last_diagnostic = diagnostic
            self._show_error(diagnostic)
        except (RuntimeError, ValueError) as error:
            diagnostic = make_diagnostic("INTERNAL_ERROR", str(error), details=str(error))
            self._last_diagnostic = diagnostic
            self._show_error(diagnostic)
        finally:
            self._update_mode_controls()


    def _upload_retained_cache(self):
        if self._mode != CACHED_MODE:
            return
        try:
            cached = self._ensure_cached_playback()
            cached.upload_retained_cache()
        except CachedPlaybackError as error:
            diagnostic = make_diagnostic(
                error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                error.message, details=error.details)
            self._last_diagnostic = diagnostic
            self._show_error(diagnostic)
        finally:
            self._update_mode_controls()

    def _cancel_cached_capture(self):
        if self._cached_playback is not None:
            self._cached_playback.cancel_capture()

    def _on_cached_playback_view(self, view):
        self._cached_view = view
        self._update_cache_text(view.cache_summary)
        if (view.state == CachedPlayback.CAPTURING and view.total
                and view.current == view.total):
            self._set_text(self._status_text, "缓存完成，正在上传…")
        elif view.state == CachedPlayback.CAPTURING and view.current == 0:
            self._set_text(self._status_text, "正在捕获缓存…")
        elif view.state == CachedPlayback.CAPTURING:
            self._set_text(
                self._status_text,
                "正在捕获缓存 {0}/{1}".format(view.current, view.total))
        elif (view.state == CachedPlayback.UPLOADING and view.total
              and view.current == view.total):
            self._set_connected(True, "缓存已发送，等待 Unreal 完整验证…")
        elif view.state == CachedPlayback.UPLOADING and view.current == 0:
            self._set_text(self._status_text, "正在上传缓存…")
        elif view.state == CachedPlayback.UPLOADING:
            self._set_text(
                self._status_text,
                "正在上传缓存 {0}/{1}".format(
                    view.current, view.total))
        elif view.state == CachedPlayback.READY:
            self._set_connected(True, "缓存已就绪，请在 Unreal 中开始播放")
        elif view.state == CachedPlayback.REPLAYING and view.current == 0:
            self._set_connected(
                True, "缓存回放中：Unreal 按捕获帧率本地播放 {0}/{1}".format(
                    view.current, view.total))
        elif view.state == CachedPlayback.REPLAYING and view.loop_round > 0 \
                and view.current > 0:
            self._set_text(
                self._status_text,
                "缓存循环播放（第 {0} 轮） {1}/{2}".format(
                    view.loop_round, view.current, view.total))
        elif view.state == CachedPlayback.REPLAYING:
            self._set_text(
                self._status_text,
                "缓存播放（仅显示已捕获缓存） {0}/{1}".format(
                    view.current, view.total))
        elif view.state == CachedPlayback.PAUSED and view.source_frame is not None:
            self._set_connected(
                True, "缓存已暂停于源帧 {0}（已应用 {1}/{2} 帧）".format(
                    view.source_frame, view.current, view.total))
        elif view.state == CachedPlayback.PAUSED:
            self._set_connected(
                True, "缓存已暂停（已应用 {0}/{1} 帧）".format(
                    view.current, view.total))
        elif view.state == CachedPlayback.SEEKED:
            self._set_connected(
                True, "已定位到源帧 {0}，缓存已暂停".format(view.source_frame))
        elif (view.state == CachedPlayback.COMPLETED
              and view.segment_start is not None):
            self._set_connected(
                True, "播放段完成（源帧 {0}–{1}），已停在最后一帧".format(
                    view.segment_start, view.segment_end))
        elif view.state == CachedPlayback.COMPLETED:
            self._set_connected(True, "缓存播放完成，已停在最后一帧")
        elif view.state == CachedPlayback.STOPPED:
            self._set_connected(True, "缓存播放已停止，已保留缓存，可再次回放")
        elif view.state == CachedPlayback.FAILED:
            # A FAILED cached view always keeps the negotiated connection: the
            # capture/upload paths that resume Real-time Preview now render as
            # REALTIME. This state still permits leaving or retrying.
            diagnostic = view.diagnostic or make_diagnostic("INTERNAL_ERROR")
            self._last_diagnostic = diagnostic
            self._set_connected(True, diagnostic["summary"])
        elif view.state == CachedPlayback.REALTIME and view.diagnostic:
            # A recoverable capture/upload/invalidation failure already resumed
            # Real-time Preview and cleared Unreal ownership; complete the same
            # transition in the UI and expose the reason, leaving the cached
            # workflow available.
            diagnostic = view.diagnostic
            self._last_diagnostic = diagnostic
            self._mode = REALTIME_MODE
            if self._session is not None:
                self._install_runtime_dispatcher(self._session)
            self._update_mode_selection()
            self._set_connected(
                self._session_ready(),
                "{0}（已恢复实时预览）".format(diagnostic["summary"]))
        elif view.state == CachedPlayback.DETACHED and view.diagnostic:
            diagnostic = view.diagnostic
            self._last_diagnostic = diagnostic
            self._set_connected(False, diagnostic["summary"])
        self._update_mode_controls()

    def _set_text(self, control, text):
        if control and cmds.control(control, exists=True):
            cmds.text(control, edit=True, label=text, annotation=text)

    def _set_scene_text(self, control, prefix, value):
        """One scene-card label, shortened to its column; the tooltip keeps it whole."""
        label, tooltip = scene_label_text(prefix, value)
        if control and cmds.control(control, exists=True):
            cmds.text(control, edit=True, label=label, annotation=tooltip)

    def _set_connected(self, connected, status):
        label = "●  已连接" if connected else "●  未连接"
        if self._light and cmds.control(self._light, exists=True):
            cmds.text(self._light, edit=True, label=label, backgroundColor=(
                LIGHT_ON_BACKGROUND if connected else LIGHT_OFF_BACKGROUND))
            self._set_style_role(self._light, "connected" if connected else "disconnected")
        self._set_text(self._status_text, status)
        self._update_mode_controls()

    def _refresh_fps(self):
        try:
            fps = frames_per_second(cmds.currentUnit(query=True, time=True))
            self._set_scene_text(self._fps_text, "帧率：", format_fps(fps))
            return fps
        except ValueError:
            unit = cmds.currentUnit(query=True, time=True)
            self._set_scene_text(self._fps_text, "帧率：", "{0}（不受支持）".format(unit))
            return None

    def _on_playback_cap_changed(self, value, *unused):
        del unused
        self._playback_cap = save_playback_cap(value)
        if self._playback_cap_menu and cmds.control(self._playback_cap_menu, exists=True):
            cmds.optionMenu(self._playback_cap_menu, edit=True, value=self._playback_cap)
        if self._session is not None and self._mode == REALTIME_MODE:
            self._session.change_cap(self._playback_cap)

    def _selected_display(self):
        selected = cmds.ls(selection=True, long=True) or []
        if len(selected) != 1:
            raise ValueError("请只选择一个 Display 曲线控制器。")
        return long_transform(selected[0])

    def _choose_enum(self, candidates):
        labels = list(candidates)
        if len(labels) == 1:
            return labels[0]
        if not labels:
            raise ValueError("所选控制器没有包含 Clothes 选项的 enum 属性。")
        result = cmds.confirmDialog(
            title="选择服装属性", message="找到多个候选属性，请选择：",
            button=labels + ["取消"], defaultButton=labels[0], cancelButton="取消",
            dismissString="取消")
        if result == "取消":
            raise ValueError("已取消 Display 属性选择。")
        return result

    def _duplicate_paths(self):
        if self._scene is None:
            return ()
        try:
            return self._scene.snapshot().duplicate_paths
        except CharacterSceneError:
            return ()

    def _refresh_duplicate_button(self):
        paths = self._duplicate_paths()
        if self._duplicate_button and cmds.control(self._duplicate_button, exists=True):
            cmds.button(
                self._duplicate_button, edit=True,
                label="选中重名骨骼（{0}）".format(len(paths)),
                enable=bool(paths))

    def select_duplicate_bones(self):
        # A diagnostic action renders the real connection state; it never
        # falsifies the indicator independently of the Streaming session.
        connected = self._session_ready()
        existing = [path for path in self._duplicate_paths() if cmds.objExists(path)]
        if not existing:
            self._refresh_duplicate_button()
            self._set_connected(connected, "重名骨骼已不存在，请重新设置角色")
            return
        cmds.select(existing, replace=True)
        self._set_connected(connected, "已选中 {0} 个重名骨骼".format(len(existing)))

    def _scene_diagnostic(self, error, default_code="ROLE_SETUP_FAILED"):
        code = error.code if error.code in DIAGNOSTICS else default_code
        diagnostic = make_diagnostic(
            code, error.message, solution=error.message, details=error.details)
        if error.duplicate_paths:
            diagnostic["duplicate_paths"] = list(error.duplicate_paths)
        return diagnostic

    def _capture_scene(self, root, display=None, display_attribute=None):
        try:
            scene = CharacterScene.capture(
                root, display=display, display_attribute=display_attribute,
                on_event=self._on_character_scene_event)
        except CharacterSceneError as error:
            if error.code == "AMBIGUOUS_DISPLAY_ATTRIBUTE" and display is not None:
                attribute = self._choose_enum(
                    error.context.get("attribute_candidates", ()))
                return self._capture_scene(root, display, attribute)
            raise
        self._scene = scene
        self._pending_root = None
        self._capture_args = (root, display, display_attribute)
        self._render_snapshot(scene.snapshot())
        return scene

    def _render_snapshot(self, snapshot):
        self._set_scene_text(self._root_text, "角色根骨骼：", snapshot.root)
        self._set_scene_text(self._outfit_text, "当前衣服：", snapshot.outfit)
        self._set_text(self._bone_text, "骨骼数：{0}".format(len(snapshot.bones)))
        self._set_text(self._curve_text, "BlendShape 数：{0}".format(
            len(snapshot.curve_names)))
        self._refresh_duplicate_button()
        self._refresh_fps()

    def set_role(self):
        require_maya()
        self._clear_role()
        try:
            root = selected_root()
            secondary = self._secondary_scene
            if secondary is not None and roots_overlap(secondary.snapshot().root, root):
                raise ValueError(
                    "主角色不能是副对象本身，也不能与副对象的骨骼层级重叠。")
            self._pending_root = root
            snapshot = self._capture_scene(root).snapshot()
            status = "角色已设置，可以连接"
            if snapshot.duplicate_paths:
                status += "（检测到 {0} 个重名骨骼，将由 UE 尝试映射）".format(
                    len(snapshot.duplicate_paths))
            status += bind_conflict_status(snapshot)
            self._set_connected(False, status)
        except CharacterSceneError as error:
            if error.code == "AMBIGUOUS_DISPLAY":
                self._pending_root = root
                self._set_scene_text(self._root_text, "角色根骨骼：", root)
                self._set_connected(
                    False, error.message + "：选中 Display 控制器后点击“选择 Display 控制器”")
            else:
                self._pending_root = None
                self._set_connected(False, error.message)
            self._show_error(self._scene_diagnostic(error))
        except (RuntimeError, ValueError) as exc:
            self._set_connected(False, str(exc))
            code = str(exc) if str(exc) in DIAGNOSTICS else "ROLE_SETUP_FAILED"
            self._show_error(make_diagnostic(code, str(exc), solution=str(exc), details=str(exc)))

    def set_display_controller(self):
        root = self._pending_root
        if root is None and self._scene is not None:
            root = self._scene.snapshot().root
        if not root:
            self._show_error(make_diagnostic(
                "INTERNAL_ERROR", "请先选择根骨骼并设置角色。"))
            return
        try:
            # Validate the requested selection first: an invalid selection
            # must never tear down the live session or a usable scene.
            display = self._selected_display()
        except (RuntimeError, ValueError) as exc:
            code = str(exc) if str(exc) in DIAGNOSTICS else "ROLE_SETUP_FAILED"
            self._show_error(make_diagnostic(code, str(exc), solution=str(exc), details=str(exc)))
            return
        try:
            # One clean termination before the replacement: the old Streaming
            # session ends while the old Character scene is still usable, so
            # a late old-session event can never close the new capture.
            self.disconnect(status="正在更换 Display 控制器，请重新连接")
            self._clear_scene(keep_pending=True)
            self._pending_root = root
            snapshot = self._capture_scene(root, display=display).snapshot()
            status = "Display 控制器已设置，可以连接"
            self._set_connected(False, status + bind_conflict_status(snapshot))
        except CharacterSceneError as error:
            self._show_error(self._scene_diagnostic(error))
    def _on_character_scene_event(self, event):
        if event.kind == "character_change_started":
            self._discard_cached_playback()
            self._outfit_change_was_connected = self._session is not None
            self.disconnect(status="衣服正在切换，正在刷新角色…")
            return
        if event.kind == "outfit_changed":
            self._render_snapshot(event.snapshot)
            if self._outfit_change_was_connected:
                status = (
                    "衣服已切换为 {0}。请在 UE 删除旧 Actor，放置新 Binding 后重新连接。"
                ).format(event.snapshot.outfit)
            else:
                status = "当前衣服已切换为 {0}。".format(event.snapshot.outfit)
            status += bind_conflict_status(event.snapshot)
            self._outfit_change_was_connected = False
            self._set_connected(False, status)
            return
        if event.kind == "character_invalidated":
            self._discard_cached_playback()
            was_connected = self._session is not None
            error = event.error
            self.disconnect(status=error.message)
            self._scene = None
            self._pending_root = None
            self._clear_scene_text()
            if was_connected:
                diagnostic = self._scene_diagnostic(error, "SAMPLING_FAILED")
                mayaapi.run_deferred(lambda: self._show_error(diagnostic, session=True),
                                     cmds.evalDeferred)

    def _on_time_unit_changed(self, *unused):
        fps = self._refresh_fps()
        if fps is None:
            if self._session is not None:
                self._session.change_rate(0.0)
            return
        try:
            validate_frame_rate(fps)
        except ValueError:
            if self._session is not None:
                self._session.change_rate(fps)
            return
        if self._session is not None:
            self._session.change_rate(fps)

    def connect(self):
        require_maya()
        if self._session is not None:
            return
        try:
            if self._scene is None:
                if self._capture_args is not None:
                    self._capture_scene(*self._capture_args)
                else:
                    self.set_role()
            if self._scene is None:
                return
            fps = validate_frame_rate(frames_per_second(cmds.currentUnit(query=True, time=True)))
        except CharacterSceneError as error:
            diagnostic = self._scene_diagnostic(error, "INTERNAL_ERROR")
            self._set_connected(False, diagnostic["summary"])
            self._show_error(diagnostic)
            return
        except (RuntimeError, ValueError) as exc:
            code = str(exc) if str(exc) in DIAGNOSTICS else "INVALID_FRAME_RATE" \
                if "frame rate" in str(exc) or "time unit" in str(exc) else "INTERNAL_ERROR"
            diagnostic = make_diagnostic(code, str(exc), details=str(exc))
            self._set_connected(False, diagnostic["summary"])
            self._show_error(diagnostic)
            return
        secondary = self._secondary_scene
        if secondary is not None and self._workflow != WORKFLOW_ANIMATION:
            diagnostic = make_diagnostic(
                "OBJECT_SET_UNSUPPORTED",
                "模型工作流只支持单个角色。",
                solution="切换到动画工作流，或在“对象配对”窗口中移除副对象后再连接。")
            self._set_connected(False, diagnostic["summary"])
            self._show_error(diagnostic)
            return
        self._last_warning = {"missing_in_unreal": [], "missing_in_maya": [],
                              "bone_name_remaps": [],
                              "has_warning": False}
        self._shown_warning_signatures = set()
        self._object_set_detail = ""
        self._set_connected(False, "正在连接 Unreal…")
        holder = {}

        def on_event(event):
            self._on_streaming_session_event(holder.get("session"), event)

        try:
            session = StreamingSession.start(
                self._scene, fps, on_event, playback_cap=self._playback_cap,
                workflow=self._workflow,
                blendshapes_enabled=self._blendshapes_enabled,
                secondary_scene=secondary)
        except StreamingSessionError as error:
            diagnostic = make_diagnostic(
                error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                error.message, details=error.details)
            if error.recapture_scene:
                self._clear_scene(keep_capture=True)
            self._set_connected(False, diagnostic["summary"])
            self._show_error(diagnostic)
            return
        holder["session"] = session
        self._session = session
        self._mode = REALTIME_MODE
        self._update_mode_controls()
        self._refresh_objects_window()

    def _on_streaming_session_event(self, session, event):
        if session is None or self._session is not session:
            return
        if event.kind == "ready":
            # Mode revisions belong to one connection.
            self._linkage_revision = 0
            self._cached_reply_sink = None
            # Cached Playback keeps the single-character contract; a two-object
            # set streams realtime and linkage only.
            if self._workflow == WORKFLOW_ANIMATION and not self._session_has_secondary():
                try:
                    cached_playback = CachedPlayback(
                        self._scene, session, on_change=self._on_cached_playback_view,
                        retention=self._cached_retention)
                except CachedPlaybackError as error:
                    diagnostic = make_diagnostic(
                        error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                        error.message, details=error.details)
                    self._last_diagnostic = diagnostic
                    self._set_connected(False, diagnostic["summary"])
                    self._show_error(diagnostic)
                    session.stop()
                    return
                self._cached_retention = None
                self._cached_playback = cached_playback
                self._on_cached_playback_view(cached_playback.view)
            # Unreal may start linkage or offer a reference while Maya
            # streams realtime, so its statements reach the panel from now on.
            self._install_runtime_dispatcher(session)
            warning = event.warning or {
                "missing_in_unreal": [], "missing_in_maya": [],
                "bone_name_remaps": [], "has_warning": False}
            self._last_warning = warning
            objects = "主角色和副对象的" if self._session_has_secondary() else ""
            if warning["has_warning"]:
                self._set_connected(
                    True, "已连接（有警告）：UE 显示{0}实时姿势，请检查差异".format(objects))
            else:
                self._set_connected(True, "已连接：UE 显示{0}实时姿势".format(objects))
            self._refresh_objects_window()
            if warning["has_warning"] \
                    and cmds.checkBox(self._warning_checkbox, query=True, value=True):
                signature = self._warning_signature(warning)
                if signature not in self._shown_warning_signatures:
                    self._shown_warning_signatures.add(signature)
                    self._show_warning(warning)
            return
        cached_playback = self._cached_playback
        if cached_playback is not None:
            self._cached_retention = cached_playback.detach(event)
            self._cached_view = cached_playback.view
        self._session = None
        # The negotiated session is gone, so linkage can no longer answer
        # targets: release the pump, the session and every borrowed resource.
        self._shutdown_linkage()
        self._mode = REALTIME_MODE
        self._refresh_objects_window()
        if event.kind == "failed":
            diagnostic = event.diagnostic or make_diagnostic("INTERNAL_ERROR")
            self._last_diagnostic = diagnostic
            if event.recapture_scene:
                self._clear_scene(keep_capture=True)
            self._set_connected(False, diagnostic["summary"])
            self._show_error(diagnostic)
            return
        if event.kind == "stopped":
            status = self._pending_stop_status or "已断开连接"
            self._pending_stop_status = None
            self._set_connected(False, status)

    def _diagnostic_text(self, diagnostic=None):
        diagnostic = diagnostic or self._last_diagnostic
        summary = diagnostic.get("summary") or "—"
        solution = diagnostic.get("solution") or "—"
        details = diagnostic.get("details") or diagnostic.get("message") or ""
        sections = [
            "摘要：{0}".format(summary),
            "解决办法：{0}".format(solution),
            "Error code: {0}".format(diagnostic.get("code", "—")),
        ]
        if details and details not in (summary, solution):
            sections.append("错误详情：\n{0}".format(details))
        sections.append(self._objects_diagnostic_text())
        sections.append(self._linkage_diagnostic_text())
        sections.append(self._reference_diagnostic_text())
        return "\n\n".join(sections)

    def _linkage_diagnostic_text(self):
        """镜头联动 read-back for 诊断详情: everything the status line leaves out."""
        status = self.linkage_status()
        labels = status.get("labels") or {}

        def value(key):
            item = status.get(key)
            return "—" if item is None else item

        age = status.get("applied_age")
        records = status.get("recovery_records") or ()
        lines = [
            "镜头联动：",
            "状态：{0}".format(status.get("line") or "—"),
            labels.get("control") or "控制方：—",
            labels.get("mode") or "模式：—",
            "mode_revision：{0} · session：{1} · set：{2} · space：{3}".format(
                value("mode_revision"), value("session_id"),
                value("set_revision"), value("space_revision")),
            labels.get("target") or "目标时间：—",
            "目标求值：{0}".format(value("target")),
            labels.get("applied") or "最新完整应用：—",
            "已应用求值：{0}".format(value("applied_eval_id")),
            "样本序号：{0}".format(value("applied_sample_serial")),
            "应用年龄：{0}".format("—" if age is None else "{0:.1f} 秒".format(age)),
            labels.get("lag") or "滞后：—",
            "已应用 / 已拒绝：{0} / {1}".format(
                status.get("applied_count") or 0, status.get("refused_count") or 0),
            "最近应用失败：{0}".format(
                "无" if status.get("apply_failure") is None
                else status.get("apply_failure") or "（Unreal 未给原因）"),
            "在途目标：{0}".format(
                (status.get("issued") or 0) + (status.get("pending") or 0)),
            "详情：{0}".format(status.get("detail") or "—"),
            "逐资源恢复：{0}".format("无" if not records else "{0} 项".format(len(records))),
        ]
        lines.extend("  {0}".format(record) for record in records)
        return "\n".join(lines)

    def _reference_diagnostic_text(self):
        """场景参考 handoff identity, frozen budgets and the full last report."""
        offer = self._reference_offer
        lines = ["场景参考："]
        if isinstance(offer, dict) and offer:
            space_revision = offer.get("space_revision")
            lines.extend([
                "交接 ID：{0}".format(offer.get("transfer_id") or "—"),
                "目录：{0}".format(offer.get("directory") or "—"),
                "范围：{0}".format(reference_scope_summary(offer)),
                "摘要：{0}".format(
                    offer.get("manifest_digest") or offer.get("digest") or "—"),
                "计数：{0}".format(reference_counts_summary(offer.get("counts"))),
                "空间版本：{0}".format(
                    "—" if space_revision is None else space_revision),
            ])
        else:
            lines.append("交接：暂无")
        budget = self.reference_budget()
        lines.append("冻结上限：字节 {0:,}（{1:g} MiB）· 对象 {2:,} · 三角面 {3:,}".format(
            int(budget["bytes"]), float(budget["bytes"]) / (1024 * 1024),
            int(budget["objects"]), int(budget["triangles"])))
        lines.append("上次结果：")
        lines.extend("  {0}".format(line)
                     for line in reference_report_lines(self.reference_report()))
        return "\n".join(lines)

    def show_diagnostics(self, *unused):
        if cmds.window(DIAGNOSTIC_WINDOW_NAME, exists=True):
            cmds.deleteUI(DIAGNOSTIC_WINDOW_NAME)
        cmds.window(
            DIAGNOSTIC_WINDOW_NAME, title="MtoU 诊断详情",
            sizeable=True, widthHeight=(620, 360))
        layout = cmds.formLayout()
        field = cmds.scrollField(
            editable=False, wordWrap=True, text=self._diagnostic_text())
        copy_button = cmds.button(
            label="复制完整诊断", command=lambda *_: self._copy_text(
                cmds.scrollField(field, query=True, text=True)))
        cmds.formLayout(
            layout, edit=True,
            attachForm=[
                (field, "top", 8),
                (field, "left", 8),
                (field, "right", 8),
                (copy_button, "left", 8),
                (copy_button, "right", 8),
                (copy_button, "bottom", 8),
            ],
            attachControl=[(field, "bottom", 6, copy_button)])
        cmds.showWindow(DIAGNOSTIC_WINDOW_NAME)

    def _copy_text(self, value):
        try:
            from PySide2 import QtWidgets
            QtWidgets.QApplication.clipboard().setText(value)
        except ImportError:
            cmds.warning("无法访问系统剪贴板。")

    def _show_warning(self, warning):
        has_blendshape_warning = bool(
            warning["missing_in_unreal"] or warning["missing_in_maya"])
        diagnostic = make_diagnostic(
            "BLENDSHAPE_MISMATCH" if has_blendshape_warning else "BONE_NAME_REMAP")
        diagnostic["details"] = (
            "Maya only: {0}\nUnreal only: {1}\nBone name remaps: {2}".format(
                ", ".join(warning["missing_in_unreal"]) or "None",
                ", ".join(warning["missing_in_maya"]) or "None",
                ", ".join(warning["bone_name_remaps"]) or "None"))
        duplicate_paths = self._duplicate_paths()
        if warning["bone_name_remaps"] and duplicate_paths:
            diagnostic["duplicate_paths"] = list(duplicate_paths)
        self._last_diagnostic = diagnostic
        buttons = ["确定", "查看详情"]
        if diagnostic.get("duplicate_paths"):
            buttons.insert(0, "选中重名骨骼")
        result = cmds.confirmDialog(
            title="MtoU 连接警告",
            message=diagnostic["summary"] + "\n\n" + diagnostic["solution"],
            button=buttons, defaultButton="确定", cancelButton="确定")
        if result == "选中重名骨骼":
            self.select_duplicate_bones()
        if result == "查看详情":
            self.show_diagnostics()

    def _show_error(self, diagnostic, session=False):
        del session
        self._last_diagnostic = diagnostic
        buttons = ["确定", "查看详情"]
        if diagnostic.get("duplicate_paths"):
            buttons.insert(0, "选中重名骨骼")
        result = cmds.confirmDialog(
            title="MtoU 连接问题", message=diagnostic["summary"] + "\n\n" + diagnostic["solution"],
            button=buttons, defaultButton="确定", cancelButton="确定")
        if result == "选中重名骨骼":
            self.select_duplicate_bones()
        if result == "查看详情":
            self.show_diagnostics()

    def _on_scene_change(self, *unused):
        self._clear_role()
        self._clear_secondary()

    def _on_before_scene_replaced(self, *unused):
        """Release the session while the scene it captured is still open.

        After the replacement the old snapshot would write its time, gate and
        camera into the new scene, so the restore happens here; the after-open
        handler then finds nothing left to restore.
        """
        del unused
        self._clear_role()
        self._clear_secondary()

    def _remove_scene_replace_callbacks(self):
        callbacks, self._scene_replace_callbacks = self._scene_replace_callbacks, []
        if om is None:
            return
        for callback_id in callbacks:
            try:
                om.MMessage.removeCallback(callback_id)
            except (AttributeError, RuntimeError, TypeError):
                pass

    def _on_maya_exiting(self, *unused):
        del unused
        self._discard_cached_playback()
        self._shutdown_linkage()
        # Maya tears the panel down after this callback; no Python object Qt
        # calls back into may outlive it.
        decorations = self._card_decorations
        self._forget_panel_widgets()
        release_panel_theme(decorations)

    def _forget_panel_widgets(self):
        self._card_decorations = []
        self._styled_controls = {}

    def _remove_maya_exit_callback(self):
        callback_id, self._maya_exit_callback = self._maya_exit_callback, None
        if callback_id is None or om is None:
            return
        try:
            om.MMessage.removeCallback(callback_id)
        except (AttributeError, RuntimeError, TypeError):
            pass

    def disconnect(self, keep_error=False, status="已断开连接"):
        del keep_error
        self._discard_cached_playback()
        # The session is about to end: linkage must restore Maya's own state
        # and drop its detail windows before the worker disappears.
        self._shutdown_linkage()
        session = self._session
        if session is None:
            self._set_connected(False, status)
            return
        self._pending_stop_status = status
        session.stop()

    def _clear_scene_text(self):
        self._set_text(self._root_text, "角色根骨骼：—")
        self._set_text(self._outfit_text, "当前衣服：—")
        self._set_text(self._bone_text, "骨骼数：0")
        self._set_text(self._curve_text, "BlendShape 数：0")
        self._refresh_duplicate_button()

    def _discard_cached_playback(self):
        cached_playback, self._cached_playback = self._cached_playback, None
        if cached_playback is not None:
            cached_playback.discard()
        self._cached_retention = None
        self._cached_view = None
        self._update_cache_text()

    def _clear_scene(self, keep_pending=False, keep_capture=False):
        self._discard_cached_playback()
        scene, self._scene = self._scene, None
        if scene is not None:
            scene.close()
        if not keep_pending:
            self._pending_root = None
        if not keep_capture:
            self._capture_args = None

    def _clear_role(self):
        self.disconnect(status="未设置角色")
        self._clear_scene()
        self._clear_scene_text()

    # ------------------------------------------------------------- linkage
    def enter_linkage(self):
        """Take exclusive Unreal time and camera control over one character.

        Refuses with an actionable diagnostic whenever the session cannot
        support linkage: no negotiated session, Cached Playback still owning
        that session, or a Maya scene the host cannot capture.
        """
        require_maya()
        if self._linkage_active():
            self._update_linkage_controls()
            return
        self._start_linkage()

    def _start_linkage(self, remote_revision=None, remote_anchor=None):
        """Enter linkage for the panel's action or for Unreal's transition.

        ``remote_revision`` is the revision of a transition Unreal started and
        ``remote_anchor`` the target placement its ``mode_state`` named.
        Every refusal of such a transition is announced to Unreal as failed,
        so it hands back the writers it borrowed instead of waiting out its
        own confirmation timeout.
        """
        session = self._session
        remote = remote_revision is not None

        def refuse(diagnostic):
            if remote:
                self._announce_remote_refusal(session, remote_revision, diagnostic)
            self._show_error(diagnostic)

        if session is None or not self._session_ready():
            refuse(make_diagnostic(
                "STREAM_INTERRUPTED",
                "镜头联动需要已连接的 Unreal 会话。",
                solution="请先设置角色并连接 Unreal，并在 UE 详情面板中为该角色配对镜头联动目标。"))
            return
        if self._cached_owns_session():
            refuse(make_diagnostic(
                "CACHE_NOT_CLEARED",
                "缓存播放仍在使用该连接。",
                solution="请先返回实时预览并等待缓存清除，再进入镜头联动。"))
            return
        if self._scene is None:
            refuse(make_diagnostic(
                "ROLE_SETUP_FAILED",
                "请先设置角色，再进入镜头联动。",
                solution="在 Maya 中选择变形根骨骼后点击“设置角色”。"))
            return
        session_id = self._session_id_of(session)
        if session_id is None:
            refuse(make_diagnostic(
                "STREAM_INTERRUPTED",
                "Unreal 尚未通告本连接的会话身份。",
                solution="请重新连接 Unreal，确认握手完成后再进入镜头联动。"))
            return
        try:
            fps = validate_frame_rate(
                frames_per_second(cmds.currentUnit(query=True, time=True)))
        except (RuntimeError, ValueError) as exc:
            refuse(make_diagnostic("INVALID_FRAME_RATE", str(exc),
                                   details=str(exc)))
            return
        object_id = getattr(session, "object_id", None) or CHARACTER_OBJECT_ID
        object_ids = list(getattr(session, "set_objects", None) or (object_id,))
        set_revision = getattr(session, "set_revision", None) or 1
        space_revision = getattr(session, "space_revision", None) or 1
        if getattr(session, "removal_pending", False):
            refuse(make_diagnostic(
                "MODE_CONFLICT",
                "正在移除副对象。",
                solution="等待 Unreal 确认移除副对象后，再进入镜头联动。"))
            return
        try:
            host = self._linkage_host(self._scene, object_ids, fps)
        except ImportError as exc:
            refuse(make_diagnostic("INTERNAL_ERROR", str(exc), details=str(exc)))
            return
        previous = self._linkage
        floor = max(self._linkage_revision,
                    previous.mode_revision() if previous is not None else 0)
        linkage = LinkageSession(
            host, session, on_event=self._on_linkage_event,
            object_id=object_id, set_revision=set_revision,
            space_revision=space_revision, mode_revision=floor,
            object_ids=object_ids)
        revision = remote_revision if remote else floor + 1
        try:
            linkage.set_session_id(session_id)
            session.begin_linkage(self._on_runtime_reply)
            linkage.enter(revision, remote=remote, anchor=remote_anchor)
        except StreamingSessionError as error:
            refuse(make_diagnostic(
                error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                error.message, details=error.details))
            return
        except Exception as error:  # the host refused; the previous mode stands
            # Leave the session's sampling path exactly as this attempt found
            # it, then report why linkage never started.
            try:
                session.resume_after_linkage()
            except (StreamingSessionError, RuntimeError, TypeError, AttributeError):
                pass
            # Unreal's statements keep reaching the panel: it may start the
            # next transition or offer a reference while Maya streams.
            self._install_runtime_dispatcher(session)
            self._report_linkage_failure(error)
            return
        self._linkage = linkage
        self._linkage_revision = linkage.mode_revision()
        self._linkage_event = None
        self._reported_linkage_failure = None
        self._mode = LINKAGE_MODE
        self._start_linkage_pump()
        self._set_connected(True, "镜头联动中：跟随 UE 序列时间与摄影机")
        self._update_linkage_status()
        self._update_mode_controls()

    def _announce_remote_refusal(self, session, revision, diagnostic):
        """Tell Unreal that Maya cannot follow the transition it started."""
        session_id = self._session_id_of(session)
        if session is None or session_id is None or not revision:
            return
        try:
            session.submit_linkage(make_mode_message(
                session_id, revision, REALTIME_MODE, CONTROL_MAYA, "failed",
                diagnostic.get("summary") or diagnostic.get("code") or ""))
        except (StreamingSessionError, RuntimeError, TypeError, ValueError,
                AttributeError):
            pass

    def _on_remote_mode_state(self, reply, received_session=None):
        """Follow one Unreal authority statement on Maya's main thread.

        The statement belongs to the connection that received it: a deferred
        statement whose connection ended, or that names another session, never
        acts on the current one.
        """
        session = self._session
        if session is None:
            return
        if received_session is not None and received_session is not session:
            return
        if exact_int(reply.get("session_id")) != self._session_id_of(session):
            return
        revision = exact_int(reply.get("mode_revision"))
        if revision is None:
            return
        linkage = self._linkage
        if linkage is None:
            if reply.get("mode") == LINKAGE_MODE and revision > self._linkage_revision:
                self._start_linkage(remote_revision=revision,
                                    remote_anchor=reply.get("anchor"))
            else:
                self._linkage_revision = max(self._linkage_revision, revision)
            return
        try:
            outcome = linkage.note_mode_state(reply)
        except LinkageError as error:
            self._report_linkage_refusal(error)
            return
        if outcome == "enter":
            self._start_linkage(remote_revision=revision,
                                remote_anchor=reply.get("anchor"))
            return
        self._linkage_revision = max(self._linkage_revision, linkage.mode_revision())
        if outcome == "left":
            self._finish_linkage_exit(
                linkage, "已按 Unreal 的指令退出镜头联动：Maya 暂停在恢复帧，连接不变")
        elif outcome == "refused":
            self._finish_linkage_exit(
                linkage, "未进入镜头联动：Maya 已恢复原状态，连接不变")
            if not linkage.recovery_records():
                # Without a residual the exit reported nothing; the refusal
                # itself is what the user must act on.
                self._report_linkage_failure(self._linkage_event.diagnostic)
        self._update_linkage_status()
        self._update_mode_controls()

    def _finish_linkage_exit(self, linkage, status):
        """Common tail of every exit Maya did not start from its own panel."""
        self._stop_linkage_pump()
        self._linkage_revision = max(self._linkage_revision, linkage.mode_revision())
        session = self._session
        if session is not None:
            try:
                session.resume_after_linkage()
            except (StreamingSessionError, RuntimeError, TypeError, AttributeError):
                pass
        self._mode = REALTIME_MODE
        records = linkage.recovery_records()
        event = self._linkage_event
        diagnostic = getattr(event, "diagnostic", None)
        detail = getattr(event, "detail", "") or ""
        if detail:
            status = "{0}（{1}）".format(status, detail)
        self._set_connected(self._session_ready(), status)
        if isinstance(diagnostic, dict) and diagnostic.get("code") and records:
            self._report_linkage_failure(diagnostic)

    def leave_linkage(self, target):
        """Return to realtime or end the preview; Maya always restores itself.

        ``realtime`` keeps the connection with the Maya timeline paused at the
        restored frame; ``end`` also tells Unreal to restore its original
        writer and keeps the connection realtime.
        """
        if target not in ("realtime", "end"):
            raise ValueError("linkage exit target must be 'realtime' or 'end'")
        linkage = self._linkage
        if linkage is None or not (linkage.is_active or target == "end"):
            self._show_error(make_diagnostic(
                "LINKAGE_NOT_ACTIVE",
                "当前不在镜头联动状态。",
                solution="请先进入镜头联动，再返回实时预览或结束预览。"))
            return
        self._stop_linkage_pump()
        try:
            residual = linkage.leave(target, linkage.mode_revision() + 1)
        except (LinkageError, StreamingSessionError) as error:
            self._report_linkage_failure(error)
            self._start_linkage_pump()
            return
        # The dispatcher stays installed for the session's lifetime: Unreal
        # answers a leave with a mode statement, and it may push a reference
        # offer while Maya is merely streaming realtime.
        session = self._session
        if session is not None:
            try:
                session.resume_after_linkage()
            except (StreamingSessionError, RuntimeError, TypeError, AttributeError):
                pass
        self._mode = REALTIME_MODE
        self._linkage_revision = max(self._linkage_revision, linkage.mode_revision())
        status = ("已返回实时预览：Maya 暂停在恢复帧，连接不变"
                  if target == "realtime"
                  else "已结束预览：Unreal 恢复原写入者，连接保持实时")
        if residual:
            status += "（恢复残留：{0}）".format("；".join(residual))
        self._set_connected(self._session_ready(), status)
        self._update_linkage_status()
        self._update_mode_controls()

    def _shutdown_linkage(self):
        """Release every linkage resource and drop the reference window."""
        self._stop_linkage_pump()
        self._close_sub_windows()
        linkage, self._linkage = self._linkage, None
        if linkage is None:
            return
        try:
            linkage.close()
        except (RuntimeError, ValueError, TypeError):
            pass
        self._linkage_last_recovery = list(linkage.recovery_records())
        self._release_reply_listener(self._session)

    def _linkage_active(self):
        linkage = self._linkage
        return bool(linkage is not None and linkage.is_active)

    def _cached_owns_session(self):
        """True while Cached Playback still owns the negotiated session."""
        return self._mode == CACHED_MODE

    def _linkage_host(self, scene, object_ids, frame_rate):
        """Build the Maya host adapter that applies linkage targets.

        One seam keeps the panel importable and testable wherever the
        Maya-only host module is unavailable.
        """
        from .link_host import MayaLinkageHost

        scenes = getattr(self._session, "object_scenes", None)
        return MayaLinkageHost(scene, object_ids, frame_rate,
                               scenes=scenes() if callable(scenes) else None)

    def _start_linkage_pump(self):
        """Apply waiting targets on Maya's main thread at a bounded rate."""
        if self._linkage is None or self._linkage_pump is not None or om is None:
            return
        self._linkage_pump_generation += 1
        generation = self._linkage_pump_generation

        def on_timer(*unused):
            del unused
            if generation != self._linkage_pump_generation:
                return
            self._pump_linkage()

        try:
            self._linkage_pump = om.MTimerMessage.addTimerCallback(
                LINKAGE_PUMP_INTERVAL, on_timer)
        except (AttributeError, RuntimeError, TypeError):
            self._linkage_pump = None

    def _stop_linkage_pump(self):
        self._linkage_pump_generation += 1
        timer_id, self._linkage_pump = self._linkage_pump, None
        if timer_id is None or om is None:
            return
        try:
            om.MMessage.removeCallback(timer_id)
        except (AttributeError, RuntimeError, TypeError):
            pass

    def _pump_linkage(self):
        """One pump step: apply the waiting target, then refresh the panel."""
        linkage = self._linkage
        if linkage is None:
            return
        session = self._session
        poll = getattr(session, "poll_connection", None)
        if callable(poll) and not poll():
            # Linkage paused live sampling, so only this pump notices that
            # Unreal ended the connection; the session's terminal event then
            # restores Maya on the main thread.
            return
        if self._reference_busy:
            # A reference action is running; the next pump step samples.
            return
        if linkage.confirmation_overdue():
            linkage.expire_unconfirmed()
            self._finish_linkage_exit(
                linkage, "Unreal 未确认镜头联动：Maya 已恢复原状态，连接不变")
            self._update_linkage_status()
            self._update_mode_controls()
            return
        try:
            linkage.pump()
        except (LinkageError, StreamingSessionError) as error:
            self._report_linkage_failure(error, repeat=True)
        except Exception as error:  # the timer must outlive one bad tick
            self._report_linkage_failure(
                make_diagnostic("INTERNAL_ERROR", str(error), details=str(error)),
                repeat=True)
        self._update_linkage_status()

    def _on_runtime_reply(self, reply):
        """Route one post-ready Unreal reply while linkage owns the session.

        Runs on the sender thread: the linkage state machine is thread-safe and
        never touches Maya here, and every panel update is deferred to Maya's
        main thread so a refusal can never kill the sender.
        """
        if not isinstance(reply, dict):
            return True
        kind = reply.get("type")
        try:
            if kind == "camera_time":
                if self._linkage_active():
                    self._linkage.offer_target(reply)
                return True
            if kind == "applied":
                if self._linkage_active():
                    self._linkage.note_applied(reply)
                return True
            if kind == "mode_state":
                # Entering and leaving touch Maya, so the statement is
                # followed on the main thread, in arrival order.
                session = self._session
                if exact_int(reply.get("session_id")) != self._session_id_of(session):
                    # A statement for another connection changes nothing.
                    return True
                self._defer_ui(lambda reply=dict(reply), session=session:
                               self._on_remote_mode_state(reply, session))
                return True
            if kind == "reference_offer":
                self.offer_reference(reply)
                return True
            if kind == "object_removed":
                self._on_object_removed(reply)
                return True
            if self._linkage is not None:
                if kind == "error":
                    self._report_runtime_error(reply)
                return True
            sink = self._cached_reply_sink
            if sink is not None:
                # Cached Playback owned this connection before: the replies it
                # received through its own queue keep going there.
                sink(reply)
                return True
            # Before any linkage the session keeps its own handling, so an
            # Unreal error still ends the realtime connection as in v9.
            return False
        except LinkageError as error:
            self._defer_ui(lambda error=error: self._report_linkage_refusal(error))
        except Exception as error:  # never crash the sender thread
            self._defer_ui(lambda error=error: self._report_linkage_failure(
                make_diagnostic("INVALID_MESSAGE", str(error), details=str(error))))
        return True

    def _install_runtime_dispatcher(self, session):
        """Route Unreal's linkage statements to the panel for this session."""
        install = getattr(session, "set_cached_reply_listener", None)
        if callable(install):
            try:
                install(self._on_runtime_reply)
            except (AttributeError, RuntimeError, TypeError):
                pass

    def _report_runtime_error(self, reply):
        """The panel's existing diagnostic path for one Unreal error reply."""
        code = reply.get("code") or "STREAM_INTERRUPTED"
        linkage = self._linkage
        if linkage is not None and (
                code in ("EVAL_STALE", "LINKAGE_NOT_ACTIVE", "SET_REVISION_STALE")
                or (code == "MODE_CONFLICT"
                    and is_scheduling_conflict(reply.get("details")))):
            # Unreal refused an answer to a target a pause, jump or camera edit
            # cancelled, a group still in flight when linkage ended, or a group
            # a mode change overtook (an older revision, or one that arrived
            # during the transition); that is normal scheduling, counted, not
            # a dialog. Every other MODE_CONFLICT is still reported.
            linkage.note_refused(reply.get("details") or reply.get("message") or "")
            return
        message = reply.get("message") or "Unreal 报告了一个错误。"
        details = reply.get("details") or reply.get("message") or ""
        diagnostic = make_diagnostic(
            code if code in DIAGNOSTICS else "INTERNAL_ERROR", message,
            details=details)
        self._last_diagnostic = diagnostic
        self._defer_ui(lambda: self._show_error(diagnostic))

    def _on_linkage_event(self, event):
        """Keep the state machine's last event; its diagnostic is the reason."""
        self._linkage_event = event

    def _report_linkage_refusal(self, error):
        """Report one LinkageError refusal and keep the session alive."""
        if getattr(error, "code", None) == "EVAL_STALE" \
                or getattr(error, "stale", False):
            # A cancelled or superseded target is normal while scrubbing, and
            # a message of a mode Maya already left is still in flight after
            # Maya's own transition; both are counted, not reported.
            linkage = self._linkage
            if linkage is not None and getattr(error, "stale", False):
                linkage.note_refused()
            return
        self._report_linkage_failure(error)

    def _report_linkage_failure(self, error, repeat=False):
        """Report one linkage failure through the panel's diagnostic path."""
        code = getattr(error, "code", None)
        message = getattr(error, "message", None) or str(error)
        details = getattr(error, "details", None) or message
        if not (isinstance(code, str) and code in DIAGNOSTICS):
            # A host refusal carries no protocol code; the state machine
            # already described it in its own failed event.
            event = getattr(self, "_linkage_event", None)
            event_diagnostic = getattr(event, "diagnostic", None)
            if isinstance(event_diagnostic, dict) and event_diagnostic.get("code"):
                code = event_diagnostic.get("code")
                message = event_diagnostic.get("message") or message
                details = event_diagnostic.get("details") or details
        diagnostic = make_diagnostic(
            code if code in DIAGNOSTICS else "INTERNAL_ERROR", message,
            details=details)
        if repeat:
            # A failing tick repeats every pump; one report per distinct reason.
            signature = (diagnostic["code"], diagnostic["details"])
            if signature == self._reported_linkage_failure:
                return
            self._reported_linkage_failure = signature
        self._last_diagnostic = diagnostic
        self._show_error(diagnostic)

    def _defer_ui(self, action):
        """Run one panel update on Maya's main thread."""
        if cmds is None:
            action()
            return
        try:
            mayaapi.run_deferred(action, cmds.evalDeferred)
        except (AttributeError, RuntimeError, TypeError):
            action()

    def _release_reply_listener(self, session):
        """Stop routing Unreal's post-ready replies to the linkage dispatcher.

        The session exposes no linkage-specific uninstall, and its public
        setter owns that route: leaving this dispatcher installed would swallow
        the error replies the streaming session itself must fail on.
        """
        release = getattr(session, "set_cached_reply_listener", None)
        if callable(release):
            try:
                release(None)
            except (AttributeError, RuntimeError, TypeError):
                pass

    def _session_id_of(self, session):
        """Unreal's session id for this connection, from the hello reply."""
        if session is None:
            return None
        reply = getattr(session, "hello_reply", None)
        if callable(reply):
            try:
                reply = reply()
            except (AttributeError, RuntimeError, TypeError):
                reply = None
        if not isinstance(reply, dict):
            return None
        return exact_int(reply.get("session_id"))

    # -------------------------------------------------------- linkage views
    def linkage_active(self):
        """True while linkage holds the exclusive time-control state."""
        return self._linkage_active()

    def linkage_status(self):
        """The authoritative linkage snapshot every window renders."""
        linkage = self._linkage
        status = linkage.status() if linkage is not None else None
        mode = status.mode if status is not None else self._mode
        control = status.control if status is not None else CONTROL_MAYA
        recovery = self._linkage_recovery()
        overdue = bool(linkage.convergence_overdue()) if linkage is not None else False
        wait = _linkage_wait_seconds(linkage) if overdue else 0.0
        held = bool(linkage.takeover_held()) if linkage is not None else False
        active = mode == LINKAGE_MODE
        line, line_tooltip = linkage_status_line(
            mode, status, recovery, overdue, wait, held)
        snapshot = {
            "active": active,
            "mode": mode,
            "control": control,
            "mode_revision": status.mode_revision if status is not None else 0,
            "session_id": status.session_id if status is not None else None,
            "set_revision": status.set_revision if status is not None else None,
            "space_revision": status.space_revision if status is not None else None,
            "target": status.target if status is not None else None,
            "target_display_frame": (
                status.target_display_frame if status is not None else None),
            "applied_eval_id": (
                status.applied_eval_id if status is not None else None),
            "applied_display_frame": (
                status.applied_display_frame if status is not None else None),
            "applied_sample_serial": (
                status.applied_sample_serial if status is not None else None),
            "applied_age": status.applied_age if status is not None else None,
            "lag_seconds": status.lag_seconds if status is not None else None,
            "pending": status.pending if status is not None else 0,
            "issued": status.issued if status is not None else 0,
            "applied_count": status.applied_count if status is not None else 0,
            "refused_count": status.refused_count if status is not None else 0,
            "apply_failure": status.apply_failure if status is not None else None,
            "converged": bool(status.converged) if status is not None else False,
            "overdue": overdue,
            "wait_seconds": wait,
            "takeover_held": held,
            "recovery": recovery,
            "recovery_records": recovery,
            "detail": (status.detail if status is not None else
                       "未进入镜头联动。"),
            "labels": _linkage_status_labels(
                mode, control, status, recovery, overdue, active),
            "line": line,
            "line_tooltip": line_tooltip,
        }
        return snapshot

    def _linkage_recovery(self):
        linkage = self._linkage
        if linkage is not None:
            return tuple(linkage.recovery_records())
        return tuple(self._linkage_last_recovery)

    def _update_linkage_status(self):
        """Render the linkage card's one status line from the current snapshot."""
        status = self.linkage_status()
        control = self._link_status_text
        if control and cmds.control(control, exists=True):
            cmds.text(control, edit=True, label=status.get("line") or "",
                      annotation=status.get("line_tooltip") or "")

    def _update_linkage_controls(self):
        """Enablement and tooltips for the linkage card's actions."""
        ready = self._session_ready()
        active = self._linkage_active()
        # 返回实时 keeps Unreal's takeover, so 结束预览 stays the explicit way
        # to hand the original writers back.
        held = bool(ready and self._linkage is not None
                    and self._linkage.takeover_held())
        cached_owner = self._cached_owns_session()
        self._set_enabled(self._enter_linkage_button,
                           ready and not cached_owner and not active)
        self._set_enabled(self._leave_linkage_button, active)
        self._set_enabled(self._end_linkage_button, active or held)
        self._set_tooltip(
            self._enter_linkage_button,
            _enter_linkage_reason(ready, cached_owner, active))
        self._set_tooltip(
            self._leave_linkage_button,
            "返回实时预览：Maya 恢复原来的时间并保持暂停，连接不变。"
            if active else "进入镜头联动后可返回实时预览。")
        self._set_tooltip(
            self._end_linkage_button,
            "结束预览：Unreal 恢复原写入者，Maya 恢复自己的状态。"
            if active else
            "结束预览：Unreal 仍保留预览接管，结束后恢复原写入者。"
            if held else "进入镜头联动后可结束预览。")
        self._set_tooltip(
            self._reference_window_button, "查看、导入、移除或恢复 UE 场景参考。")
        self._set_tooltip(
            self._objects_window_button,
            "设置或移除与主角色一起预览的副对象（当前：主角色和副对象）。"
            if self._secondary_scene is not None else
            "设置或移除与主角色一起预览的副对象（当前：只有主角色）。")

    # ---------------------------------------------------------- linkage UI
    def open_reference_window(self):
        """Show the scene-reference detail window, reusing the instance."""
        window = self._reference_window
        if window is not None and window.exists():
            window.show()
            return window
        self._reference_window = None
        if cmds is not None and cmds.window(REFERENCE_WINDOW_NAME, exists=True):
            cmds.deleteUI(REFERENCE_WINDOW_NAME)
        window = ReferenceWindow(self)
        self._reference_window = window
        return window

    def close_reference_window(self):
        """Delete this detail window only; the reference state survives."""
        window, self._reference_window = self._reference_window, None
        if window is not None:
            window.close()

    def reference_window_closed(self, window):
        """Maya closed ``window`` itself: forget it without deleting anything."""
        if self._reference_window is window:
            self._reference_window = None

    def _close_sub_windows(self):
        self.close_reference_window()

    # ------------------------------------------------------------ object set
    def _session_has_secondary(self):
        """True while the connection's negotiated set holds the second object."""
        return bool(getattr(self._session, "secondary_scene", None))

    def _primary_root(self):
        snapshot = getattr(self._scene, "snapshot", None)
        if callable(snapshot):
            try:
                return snapshot().root
            except CharacterSceneError:
                return None
        return self._pending_root

    def set_secondary_object(self):
        """Capture the selected root as the second object of the set.

        The set is negotiated when the connection starts, so a change while
        connected ends the connection and the user connects again.
        """
        require_maya()
        try:
            root, display = selected_secondary_root()
            primary = self._primary_root()
            if primary and roots_overlap(primary, root):
                raise ValueError(
                    "副对象不能是主角色本身，也不能在主角色的骨骼层级里。")
        except (RuntimeError, ValueError) as exc:
            self._show_error(make_diagnostic(
                "ROLE_SETUP_FAILED", str(exc), solution=str(exc), details=str(exc)))
            return
        if self._session is not None:
            self.disconnect(status="对象集合已变更：请重新连接")
        self._clear_secondary()
        try:
            scene = self._capture_secondary(root, display)
        except CharacterSceneError as error:
            self._show_error(self._scene_diagnostic(error))
            self._refresh_objects_window()
            return
        self._secondary_scene = scene
        snapshot = scene.snapshot()
        self._set_connected(
            self._session_ready(),
            "副对象已设置（{0} 根骨骼），连接后与主角色一起预览".format(len(snapshot.bones)))
        self._refresh_objects_window()

    def _capture_secondary(self, root, display=None, display_attribute=None):
        try:
            return CharacterScene.capture(
                root, display=display, display_attribute=display_attribute,
                on_event=self._on_secondary_scene_event, display_optional=True)
        except CharacterSceneError as error:
            if error.code == "AMBIGUOUS_DISPLAY_ATTRIBUTE" and display is not None:
                attribute = self._choose_enum(
                    error.context.get("attribute_candidates", ()))
                return self._capture_secondary(root, display, attribute)
            raise

    def remove_secondary_object(self):
        """Remove the second object; inside a connection Unreal restores it first."""
        if self._secondary_scene is None:
            self._refresh_objects_window()
            return
        session = self._session
        if session is None or not self._session_has_secondary():
            self._clear_secondary()
            self._set_connected(self._session_ready(), "已移除副对象")
            self._refresh_objects_window()
            return
        if getattr(session, "removal_pending", False):
            return
        try:
            session.request_secondary_removal(
                self._session_id_of(session), "removed in the Maya pairing window")
        except StreamingSessionError as error:
            self._show_error(make_diagnostic(
                error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                error.message, details=error.details))
            return
        if self._linkage is not None:
            self._linkage.begin_set_removal()
        self._set_connected(True, "正在移除副对象，等待 Unreal 恢复它原来的状态…")
        self._refresh_objects_window()

    def _on_object_removed(self, reply):
        """Unreal confirmed a removal (Maya's or its own user's); sender thread.

        The linkage state adopts the new set at once, so the next target is
        answered by the new set; the scene and panel follow on Maya's main
        thread.
        """
        try:
            validate_reply(reply)
        except ValueError as error:
            self._defer_ui(lambda error=error: self._report_linkage_failure(
                make_diagnostic("INVALID_MESSAGE", str(error), details=str(error))))
            return
        session = self._session
        linkage = self._linkage
        if linkage is not None and reply.get("object_id") == SECONDARY_OBJECT_ID:
            try:
                linkage.adopt_set(reply.get("set_revision"), [CHARACTER_OBJECT_ID])
            except LinkageError as error:
                self._defer_ui(lambda error=error: self._report_linkage_failure(error))
        self._defer_ui(lambda reply=dict(reply), session=session:
                       self._finish_object_removal(reply, session))

    def _finish_object_removal(self, reply, session):
        if session is None or session is not self._session:
            return
        try:
            removed = session.adopt_removal(reply)
        except StreamingSessionError as error:
            self._show_error(make_diagnostic(
                error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
                error.message, details=error.details))
            return
        if removed is not None and removed is self._secondary_scene:
            self._clear_secondary()
        detail = reply.get("detail") or ""
        if reply.get("restored"):
            self._object_set_detail = "已移除副对象，Unreal 已恢复它原来的状态"
            status = "已移除副对象：主角色继续预览"
        else:
            self._object_set_detail = "移除副对象未能完整恢复：{0}".format(detail or "—")
            status = self._object_set_detail
        if self._workflow == WORKFLOW_ANIMATION and self._cached_playback is None:
            try:
                cached_playback = CachedPlayback(
                    self._scene, session, on_change=self._on_cached_playback_view,
                    retention=self._cached_retention)
            except CachedPlaybackError:
                cached_playback = None
            if cached_playback is not None:
                self._cached_retention = None
                self._cached_playback = cached_playback
                self._on_cached_playback_view(cached_playback.view)
        self._set_connected(self._session_ready(), status)
        self._update_linkage_status()
        self._refresh_objects_window()

    def _clear_secondary(self):
        scene, self._secondary_scene = self._secondary_scene, None
        if scene is not None:
            scene.close()
        self._refresh_objects_window()

    def _on_secondary_scene_event(self, event):
        """The second object changed: the negotiated set no longer describes it."""
        if event.kind == "character_change_started":
            if self._session_has_secondary():
                self.disconnect(status="副对象的衣服正在切换：连接已结束，请重新连接")
            return
        if event.kind == "outfit_changed":
            self._refresh_objects_window()
            return
        if event.kind == "character_invalidated":
            was_connected = self._session_has_secondary()
            error = event.error
            self._secondary_scene = None
            if was_connected:
                self.disconnect(status="副对象已删除或卸载：连接已结束，请重新连接")
                diagnostic = self._scene_diagnostic(error, "SAMPLING_FAILED")
                mayaapi.run_deferred(lambda: self._show_error(diagnostic, session=True),
                                     cmds.evalDeferred)
            self._refresh_objects_window()

    def objects_state(self):
        """The object set the pairing window renders; one source for every view."""
        session = self._session
        scene = self._secondary_scene
        state = {"primary": self._primary_root(), "secondary": None,
                 "secondary_detail": "副对象骨骼：—", "state": "只连接主角色",
                 "state_tooltip": "", "set_revision": getattr(session, "set_revision", None)}
        if scene is not None:
            try:
                snapshot = scene.snapshot()
                state["secondary"] = snapshot.root
                outfit = snapshot.outfit or "无 Display 控制器"
                state["secondary_detail"] = "副对象：{0} 根骨骼 · BlendShape {1} · 衣服 {2}".format(
                    len(snapshot.bones), len(snapshot.curve_names), outfit)
            except CharacterSceneError as error:
                state["secondary_detail"] = "副对象：{0}".format(error.message)
        negotiated = self._session_has_secondary()
        if getattr(session, "removal_pending", False):
            state["state"] = "正在移除副对象，等待 Unreal 确认"
        elif negotiated and self._session_ready():
            state["state"] = "已连接：主角色和副对象一起预览"
            state["state_tooltip"] = "移除副对象后主角色继续预览，不需要重新连接。"
        elif negotiated:
            state["state"] = "正在连接主角色和副对象…"
        elif scene is not None and self._workflow != WORKFLOW_ANIMATION:
            state["state"] = "模型工作流只支持单个角色"
            state["state_tooltip"] = "切换到动画工作流，或移除副对象后再连接。"
        elif scene is not None and session is not None:
            state["state"] = "副对象不在当前连接中：重新连接后生效"
        elif scene is not None:
            state["state"] = "已设置副对象：连接后与主角色一起预览"
        elif self._object_set_detail:
            state["state"] = self._object_set_detail
        return state

    def objects_window_controls(self):
        """Enablement and tooltips of the pairing window's actions."""
        session = self._session
        pending = bool(getattr(session, "removal_pending", False))
        negotiated = self._session_has_secondary()
        set_tooltip = ("先选择副对象的变形根骨骼（可同时选中它的 Display 控制器）。"
                       if session is None else
                       "设置副对象会结束当前连接，之后需要重新连接。")
        if negotiated:
            remove_tooltip = "在当前连接内移除副对象：Unreal 恢复它原来的动画后，主角色继续预览。"
        elif self._secondary_scene is not None:
            remove_tooltip = "清除副对象设置。"
        else:
            remove_tooltip = "当前没有副对象。"
        return {
            "set": (not pending, "正在移除副对象，请稍候。" if pending else set_tooltip),
            "remove": (self._secondary_scene is not None and not pending,
                       "正在移除副对象，请稍候。" if pending else remove_tooltip),
        }

    def _objects_diagnostic_text(self):
        """对象集合 for 诊断详情: the negotiated set, its revision and the removal.

        Scene paths stay in the pairing window; the diagnostics name only the
        set identity the two hosts negotiated.
        """
        state = self.objects_state()
        session = self._session
        objects = getattr(session, "set_objects", None)
        lines = [
            "对象集合：",
            "副对象：{0}".format("已设置" if self._secondary_scene is not None else "未设置"),
            "状态：{0}".format(state.get("state") or "—"),
            "连接中的对象：{0} · set：{1}".format(
                "、".join(objects) if objects else "—",
                "—" if state.get("set_revision") is None else state.get("set_revision")),
        ]
        if self._object_set_detail:
            lines.append("上次移除：{0}".format(self._object_set_detail))
        return "\n".join(lines)

    def open_objects_window(self):
        """Show the object pairing window, reusing the instance."""
        window = self._objects_window
        if window is not None and window.exists():
            window.show()
            return window
        self._objects_window = None
        if cmds is not None and cmds.window(OBJECTS_WINDOW_NAME, exists=True):
            cmds.deleteUI(OBJECTS_WINDOW_NAME)
        window = ObjectsWindow(self)
        self._objects_window = window
        return window

    def close_objects_window(self):
        """Delete the pairing window only; the object set survives."""
        window, self._objects_window = self._objects_window, None
        if window is not None:
            window.close()

    def objects_window_closed(self, window):
        """Maya closed ``window`` itself: forget it without deleting anything."""
        if self._objects_window is window:
            self._objects_window = None

    def _refresh_objects_window(self):
        window = self._objects_window
        if window is not None and window.exists():
            window.refresh()

    def _refresh_reference_window(self):
        window = self._reference_window
        if window is not None:
            window.refresh()

    # -------------------------------------------------------- scene reference
    def offer_reference(self, offer):
        """Store the handoff descriptor Unreal offered for import."""
        self._reference_offer = _copy_reference_offer(offer)
        self._defer_ui(self._refresh_reference_window)

    def reference_offer(self):
        """The stored handoff offer, or None when nothing was offered."""
        offer = self._reference_offer
        return dict(offer) if isinstance(offer, dict) else None

    def reference_budget(self):
        """The frozen scene-reference budgets."""
        return {"bytes": MAX_REFERENCE_BYTES,
                "objects": MAX_REFERENCE_OBJECTS,
                "triangles": MAX_REFERENCE_TRIANGLES}

    def reference_report(self):
        """The last reference action, its report, handoff and finish time."""
        outcome = self._reference_report
        return {"action": outcome.get("action"),
                "report": dict(outcome.get("report") or {}),
                "transfer_id": outcome.get("transfer_id"),
                "time": outcome.get("time")}

    def reference_state(self):
        """Return ``(state, tooltip)``: whether the offered handoff can be imported."""
        offer = self._reference_offer
        if not isinstance(offer, dict) or not offer:
            return "暂无交接", "先在 UE 的“场景参考”中导出，再回到这里导入。"
        outcome = self._reference_report
        report = outcome.get("report") or {}
        current = (outcome.get("action") == "import"
                   and outcome.get("transfer_id") == offer.get("transfer_id"))
        session_space = exact_int(getattr(self._session, "space_revision", None))
        offer_space = exact_int(offer.get("space_revision"))
        if (current and report.get("code") == "REFERENCE_STALE") or (
                session_space is not None and offer_space is not None
                and offer_space != session_space):
            return ("已过期，请在 UE 重新导出",
                    "这份交接属于旧的共同空间；在 UE 重新导出场景参考后再导入。")
        if current and report.get("accepted"):
            return ("已导入（{0}）".format(import_time_label(outcome.get("time"))),
                    "再次点击“导入/更新参考”会用这份交接替换场景中的参考。")
        return "可导入", "点击“导入/更新参考”把这份交接导入 Maya。"

    def reference_window_controls(self):
        """Enablement and tooltips for the reference window's buttons."""
        ready = self._session_ready()
        offer = self._reference_offer
        import_enabled = bool(offer) and ready
        if import_enabled:
            import_tooltip = "按冻结上限校验并导入 UE 交接的场景参考。"
        elif not offer:
            import_tooltip = "先在 Unreal 中导出场景参考交接。"
        else:
            import_tooltip = "需要已连接的 Unreal 会话来汇报导入结果。"
        return {
            "import": (import_enabled, import_tooltip),
            "remove": (True, "删除工具自己创建的参考容器，不触碰其他节点。"),
            "recover": (True, "完成或放弃上一次被中断的参考更新。"),
        }

    def import_reference(self):
        """Validate one offered handoff, then import it under the budgets."""
        require_maya()
        handoff = self._reference_handoff()
        if handoff is None:
            self._warn("没有待导入的场景参考交接。")
            return
        if not self._session_ready():
            self._show_error(make_diagnostic(
                "STREAM_INTERRUPTED",
                "导入场景参考需要已连接的 Unreal 会话。",
                solution="请先连接 Unreal，再导入它交接的参考。"))
            return
        reference = self._reference_module_or_report()
        if reference is None:
            return
        budget = self.reference_budget()
        try:
            with self._holding_live_samples():
                report = reference.inspect(handoff, budget)
        except reference.ReferenceRefused as error:
            self._report_reference_refusal(error, record=True)
            return
        if not (isinstance(report, dict) and report.get("accepted")):
            # Validation reported a problem instead of raising it: nothing is
            # staged, and the report card carries the reason.
            self._reference_report = self._reference_outcome("import", report)
            self._warn("场景参考未通过校验：{0}".format(
                (report or {}).get("detail") if isinstance(report, dict) else "未知原因"))
            self._refresh_reference_window()
            return
        try:
            with self._holding_live_samples():
                report = reference.import_handoff(handoff, budget)
        except reference.ReferenceRefused as error:
            self._report_reference_refusal(error, record=True)
            return
        self._reference_report = self._reference_outcome("import", report)
        self._submit_reference_result(report)
        self._set_text(self._status_text, self._reference_outcome_text(report))
        self._refresh_reference_window()

    def remove_reference(self):
        """Remove only the tool-owned reference container."""
        require_maya()
        reference = self._reference_module_or_report()
        if reference is None:
            return
        try:
            with self._holding_live_samples():
                report = self._reference_container(reference).remove()
        except reference.ReferenceRefused as error:
            self._report_reference_refusal(error)
            return
        self._reference_report = self._reference_outcome("remove", report)
        self._submit_reference_removed(report)
        self._set_text(self._status_text, self._reference_outcome_text(report))
        self._refresh_reference_window()

    def recover_reference(self):
        """Finish or discard the interrupted reference update."""
        require_maya()
        reference = self._reference_module_or_report()
        if reference is None:
            return
        try:
            with self._holding_live_samples():
                report = self._reference_container(reference).recover()
        except reference.ReferenceRefused as error:
            self._report_reference_refusal(error)
            return
        self._reference_report = self._reference_outcome("recover", report)
        self._set_text(self._status_text, self._reference_outcome_text(report))
        self._refresh_reference_window()

    @contextlib.contextmanager
    def _holding_live_samples(self):
        """No live sample or linkage group while a reference action runs.

        The FBX import and the container swap are long Maya commands during
        which Maya can run this tool's timers; a pose read from the half-built
        scene would fail and end the connection.
        """
        self._reference_busy += 1
        hold = getattr(self._session, "holding_samples", None)
        try:
            with hold() if callable(hold) else contextlib.nullcontext():
                yield
        finally:
            self._reference_busy -= 1

    def _reference_module_or_report(self):
        """The scene-reference module, or None after reporting why it is gone."""
        try:
            from . import reference
        except ImportError as error:
            self._show_error(make_diagnostic("INTERNAL_ERROR", str(error),
                                             details=str(error)))
            return None
        return reference

    def _reference_container(self, reference):
        offer = self._reference_offer
        space_revision = offer.get("space_revision") if offer else None
        return reference.ReferenceContainer(
            container=REFERENCE_CONTAINER, space_revision=space_revision,
            budget=self.reference_budget())

    def _reference_handoff(self):
        """Map the stored offer onto the reference module's handoff shape."""
        offer = self._reference_offer
        if not isinstance(offer, dict) or not offer:
            return None
        handoff = {
            "transfer_id": offer.get("transfer_id"),
            "directory": offer.get("directory"),
            "digest": offer.get("manifest_digest"),
            "space_revision": offer.get("space_revision"),
        }
        scope = offer.get("scope")
        if isinstance(scope, dict):
            handoff["scope"] = dict(scope)
        counts = offer.get("counts")
        if isinstance(counts, dict):
            handoff["counts"] = dict(counts)
        return handoff

    def _submit_reference_result(self, report):
        """Tell Unreal how the local import, swap or removal ended."""
        session = self._session
        session_id = self._session_id_of(session)
        if session is None or session_id is None:
            self._warn("场景参考结果无法发送给 Unreal：没有已连接的会话。")
            return
        offer = self._reference_offer or {}
        counts = report.get("counts") if isinstance(report, dict) else {}
        container = report.get("container") if isinstance(report, dict) else {}
        container_name = container.get("name") if isinstance(container, dict) else None
        message = make_reference_result_message(
            session_id, offer.get("transfer_id") or "",
            bool(report.get("accepted")), str(report.get("detail") or ""),
            counts=counts if isinstance(counts, dict) else {},
            container=container_name,
            code=report.get("code") if isinstance(report, dict) else None)
        try:
            session.submit_linkage(message)
        except (StreamingSessionError, RuntimeError, TypeError, ValueError) as error:
            self._warn("场景参考结果发送失败：{0}".format(error))

    def _submit_reference_removed(self, report):
        """Tell Unreal the tool-owned container is gone."""
        session = self._session
        session_id = self._session_id_of(session)
        if session is None or session_id is None:
            self._warn("移除结果无法发送给 Unreal：没有已连接的会话。")
            return
        message = make_reference_removed_message(
            session_id, str((report or {}).get("detail") or ""))
        try:
            session.submit_linkage(message)
        except (StreamingSessionError, RuntimeError, TypeError, ValueError) as error:
            self._warn("移除结果发送失败：{0}".format(error))

    def _reference_outcome(self, action, report):
        """One stored reference result, tied to the offered handoff and its time."""
        offer = self._reference_offer or {}
        return {"action": action, "report": report,
                "transfer_id": offer.get("transfer_id"), "time": time.time()}

    def _report_reference_refusal(self, error, record=False):
        code = getattr(error, "code", None) or "INTERNAL_ERROR"
        detail = getattr(error, "detail", None) or str(error)
        if record:
            # A refused import is the window's last result too, so its state
            # line can say a stale handoff needs a new export.
            self._reference_report = self._reference_outcome(
                "import", {"accepted": False, "code": code, "detail": detail})
            self._refresh_reference_window()
        diagnostic = make_diagnostic(
            code if code in DIAGNOSTICS else "INTERNAL_ERROR", detail,
            details=detail)
        self._last_diagnostic = diagnostic
        self._show_error(diagnostic)

    def _reference_outcome_text(self, report):
        """One status line for the main panel after a reference action."""
        if not isinstance(report, dict):
            return "场景参考操作已完成。"
        if report.get("rolled_back"):
            return "场景参考：导入失败已回滚（{0}）".format(report.get("detail") or "")
        if report.get("accepted"):
            counts = report.get("counts") or {}
            text = "场景参考：已导入 {0} 个对象".format(counts.get("objects") or 0)
            anchor = report.get("anchor")
            if isinstance(anchor, dict) and anchor.get("source") == "absent":
                text += "；交接清单未带锚点，参考位于 Maya 原点，请在 Unreal 重新导出并再次导入"
            return text
        if report.get("removed"):
            return "场景参考：已移除 {0} 个节点".format(report.get("nodes_removed") or 0)
        detail = report.get("detail")
        return "场景参考：{0}".format(detail) if detail else "场景参考操作已完成。"

    @staticmethod
    def _warn(message):
        if cmds is None:
            return
        try:
            cmds.warning("MtoU_LiveLink {0}".format(message))
        except (AttributeError, RuntimeError, TypeError):
            pass

    def close(self, *unused):
        """Maya is closing the panel: release what it owns, never delete it again."""
        self._remove_maya_exit_callback()
        self._remove_scene_replace_callbacks()
        self._clear_role()
        self._clear_secondary()
        self.close_objects_window()
        # Qt deletes the decorations with the panel.
        self._forget_panel_widgets()

CONTROLLER = None

WINDOW_NAME = "MtoULiveLinkWindow"

DIAGNOSTIC_WINDOW_NAME = "MtoULiveLinkDiagnosticWindow"

# The pump applies one waiting target per tick and refreshes the panel; the
# rate is a ceiling, not a frame-rate target, and the callback always runs on
# Maya's main thread.
LINKAGE_PUMP_INTERVAL = 1.0 / 60.0

LINKAGE_MODE_LABELS = {
    REALTIME_MODE: "实时预览",
    CACHED_MODE: "缓存播放",
    LINKAGE_MODE: "镜头联动",
}

LINKAGE_CONTROL_LABELS = {CONTROL_MAYA: "Maya", CONTROL_UNREAL: "Unreal"}

# The cache summary sits in the scene card's right column. Its label has a
# fixed width so a long summary can never widen the panel, and its text is kept
# within CACHE_LABEL_UNITS (a CJK character counts two units, anything else
# one) so it is not clipped; the tooltip keeps the whole summary.
CACHE_TEXT_INDENT = 16

CACHE_TEXT_WIDTH = CARD_HALF - CACHE_TEXT_INDENT

CACHE_LABEL_UNITS = 26

_text_units = text_units

# The scene card's left column holds the character root, the outfit and the
# frame rate at the cache summary's fixed width, so a long DAG path or outfit
# name is shortened instead of widening the panel. Its labels carry no "缓存："
# style suffix, so their budget is measured on the panel font with a margin.
SCENE_TEXT_WIDTH = CACHE_TEXT_WIDTH

SCENE_LABEL_UNITS = 29

# The linkage card's one status line spans the card; its text is kept within
# LINK_STATUS_UNITS and the tooltip carries the whole line and what to do.
LINK_STATUS_WIDTH = CARD_CONTENT - 2 * CARD_MARGIN

LINK_STATUS_UNITS = 2 * CACHE_LABEL_UNITS + 4

# The applied lag the status line reports: below it Maya keeps up and the line
# says 同步正常. Display only; the protocol's convergence window is unchanged.
LINKAGE_LAG_NOTICE_SECONDS = 0.5


def selected_secondary_root():
    """Return ``(root, display)`` for the second object from Maya's selection.

    One deformation root joint, optionally with its Display controller when
    the object has several; a prop without one is fine.
    """
    selected = cmds.ls(selection=True, long=True) or []
    joints = [path for path in selected if cmds.nodeType(path) == "joint"]
    others = [path for path in selected if path not in joints]
    if len(joints) != 1 or len(others) > 1:
        raise ValueError(
            "请只选择副对象的一个变形根骨骼（可同时选中它的 Display 控制器）。")
    display = long_transform(others[0]) if others else None
    return joints[0], display


def roots_overlap(first, second):
    """True when one DAG root is the other or lies below it."""
    first = str(first or "")
    second = str(second or "")
    if not first or not second:
        return False
    return (first == second or first.startswith(second + "|")
            or second.startswith(first + "|"))


def scene_label_text(prefix, value):
    """Return ``(label, tooltip)`` for one scene-card label (R-020).

    The value is shortened in the middle, so both the start and the leaf of a
    DAG path or outfit name stay readable; the tooltip keeps the whole value.
    """
    value = "—" if value in (None, "") else str(value)
    full = prefix + value
    budget = SCENE_LABEL_UNITS - _text_units(prefix)
    return prefix + elide_text(value, budget, middle=True), full


def cache_summary_text(summary):
    """Return ``(label, tooltip)`` for the scene card's cache summary."""
    if summary is None:
        return "缓存：无", "缓存：无"
    start, end = summary.capture_range
    capture_time = time.strftime(
        "%Y-%m-%d %H:%M:%S", time.localtime(summary.capture_time))
    tooltip = "缓存：{0}-{1}，{2} 帧，{3:g} fps，捕获于 {4}".format(
        start, end, summary.frame_count, summary.scene_fps, capture_time)
    for label in ("缓存：{0}-{1}（{2} 帧）".format(start, end, summary.frame_count),
                  "缓存：{0}-{1}".format(start, end)):
        if _text_units(label) <= CACHE_LABEL_UNITS:
            return label, tooltip
    tail = (CACHE_LABEL_UNITS - _text_units("缓存：…")) // 2
    return "缓存：{0}…{1}".format(label[3:3 + tail], label[-tail:]), tooltip


def _linkage_wait_seconds(linkage):
    wait = getattr(linkage, "wait_seconds", None)
    try:
        return float(wait()) if callable(wait) else 0.0
    except (RuntimeError, TypeError, ValueError):
        return 0.0


def linkage_status_line(mode, status, recovery, overdue, wait, held):
    """Return ``(line, tooltip)``: the linkage card's one status line.

    Only the state, a lag Maya cannot keep up with, and a failed restore are
    shown; the evaluation, sample and revision read-back stays in 诊断详情.
    """
    hints = []
    if mode == LINKAGE_MODE:
        parts = ["镜头联动中", "Unreal 控制时间"]
        lag = status.lag_seconds if status is not None else None
        applied = status is not None and status.applied_eval_id
        # An overdue target is the lag in effect: it has waited longer than
        # the last complete application took.
        behind = float(wait) if overdue else (
            float(lag) if lag is not None else 0.0)
        failure = status.apply_failure if status is not None else None
        if failure is not None:
            # Unreal could not complete the latest group: Maya is not the cause,
            # and the last complete application stays what Unreal shows.
            parts.append("应用失败，重试 {0} 秒".format(_seconds_label(behind))
                         if overdue else "应用失败，重试中")
            hints.append("Unreal 拒绝了最新一组姿势{0}；Maya 会自动重试。"
                         "持续失败时打开“诊断详情”查看原因。".format(
                             "（{0}）".format(failure) if failure else ""))
        elif overdue or behind >= LINKAGE_LAG_NOTICE_SECONDS:
            parts.append("Maya 跟不上，滞后 {0} 秒".format(_seconds_label(behind)))
            hints.append("Maya 正在追赶 Unreal 的最新时间；可暂停 Sequencer 等待，"
                         "或关闭占用性能的程序。")
        elif not applied:
            parts.append("正在同步")
        else:
            parts.append("同步正常")
    else:
        parts = ["未进入联动"]
        if mode == CACHED_MODE:
            parts.append("缓存播放中")
        if held:
            parts.append("Unreal 仍接管目标")
            hints.append("点击“结束预览”让 Unreal 交还原来的动画。")
    if recovery:
        parts.append("恢复失败 {0} 项".format(len(recovery)))
        hints.append("打开“诊断详情”查看未恢复的项目，按记录在 Maya 中手动恢复"
                     "（例如解锁相机或重设时间范围）。")
    line = " · ".join(parts)
    tooltip = "\n".join([line] + hints)
    return elide_text(line, LINK_STATUS_UNITS), tooltip


def _seconds_label(value):
    return "{0:.1f}".format(value) if value < 10 else "{0:.0f}".format(value)


def _frame_label(value):
    if value is None:
        return "—"
    return "{0:.2f}".format(float(value))


def _linkage_status_labels(mode, control, status, recovery, overdue, active):
    """The status row's labels; one source for the card and the window."""
    target, applied, lag_label = "—", "—", "—"
    if active:
        target = _frame_label(
            status.target_display_frame if status is not None else None)
        applied = _frame_label(
            status.applied_display_frame if status is not None else None)
        lag = status.lag_seconds if status is not None else None
        if lag is not None:
            lag_label = "{0:.2f} 秒".format(float(lag))
            if overdue:
                lag_label += "（未收敛）"
    return {
        "control": "控制方：{0}".format(
            LINKAGE_CONTROL_LABELS.get(control, "—")),
        "mode": "模式：{0}".format(LINKAGE_MODE_LABELS.get(mode, "—")),
        "target": "目标时间：{0}".format(target),
        "applied": "最新完整应用：{0}".format(applied),
        "lag": "滞后：{0}".format(lag_label),
        "recovery": "恢复结果：{0}".format(
            "无" if not recovery else "；".join(recovery)),
    }


def _enter_linkage_reason(ready, cached_owner, active):
    """Why 进入联动 is disabled, in the tooltip the button carries."""
    if active:
        return "镜头联动已在进行中。"
    if cached_owner:
        return "缓存播放正在使用该连接：请先返回实时预览并等待缓存清除。"
    if not ready:
        return "请先设置角色并连接 Unreal，并在 UE 中配对镜头联动目标。"
    return ""


def _copy_reference_offer(offer):
    """One stored copy of the offered handoff, safe to read from any thread."""
    if not isinstance(offer, dict):
        return None
    copied = dict(offer)
    for name in ("scope", "counts"):
        value = copied.get(name)
        if isinstance(value, dict):
            copied[name] = dict(value)
    return copied
