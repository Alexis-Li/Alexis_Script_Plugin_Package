"""Product controller: panel, session and scene state authority."""

import time

from .mayaapi import cmds, om, omui

from .cache import CachedPlayback, CachedPlaybackError
from .config import CACHED_MODE, DEFAULT_PLAYBACK_CAP, PLAYBACK_CAP_CHOICES, REALTIME_MODE, WORKFLOWS, WORKFLOW_ANIMATION, WORKFLOW_MODEL
from .diagnostics import DIAGNOSTICS, make_diagnostic
from .scene import CharacterScene, CharacterSceneError, long_transform, require_maya, selected_root
from .session import StreamingSession, StreamingSessionError, bind_conflict_status
from .space import format_fps, frames_per_second, validate_frame_rate
from .timeline import load_playback_cap, save_playback_cap
from .ui.style import BUTTON_HEIGHT, CARD_CONTENT, CARD_GAP, CARD_HALF, CARD_MARGIN, GRID_GAP, LIGHT_OFF_BACKGROUND, LIGHT_ON_BACKGROUND, PANEL_MARGIN, PANEL_STYLE, PANEL_WIDTH, PRIMARY_BACKGROUND, ROW_SPACING, TAB_HEIGHT, TAB_OFF_BACKGROUND, TAB_ON_BACKGROUND, TOGGLE_OFF_BACKGROUND, TOGGLE_ON_BACKGROUND


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

    @staticmethod
    def _grid_row(controls, spans=None, height=BUTTON_HEIGHT, top=0):
        """Attach controls of the current formLayout to equal grid columns.

        ``spans`` gives each control's column count (default one each). Every
        row shares the same columns and GRID_GAP gutter, so edges line up.
        ``top`` insets the row from the top of its own height.
        """
        form = cmds.setParent(query=True)
        spans = spans or [1] * len(controls)
        columns = sum(spans)
        positions, attach_form, start = [], [], 0
        for control, span in zip(controls, spans):
            end = start + span
            positions.append((control, "left", 0 if start == 0 else GRID_GAP // 2,
                              start * 100 // columns))
            positions.append((control, "right", 0 if end == columns else GRID_GAP // 2,
                              end * 100 // columns))
            attach_form.append((control, "top", top))
            start = end
        cmds.formLayout(form, edit=True, height=height, attachPosition=positions,
                        attachForm=attach_form)
        cmds.setParent("..")
        return form

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
        self._root_text = cmds.text(label="角色根骨骼：—", align="left")
        self._outfit_text = cmds.text(label="当前衣服：—", align="left")
        self._fps_text = cmds.text(label="场景帧率：—", align="left")
        cmds.setParent("..")
        cmds.columnLayout(adjustableColumn=False, rowSpacing=6,
                          columnAttach=("left", 16))
        self._bone_text = cmds.text(label="骨骼数：0", align="left")
        self._curve_text = cmds.text(label="BlendShape 数：0", align="left")
        self._cache_text = cmds.text(label="缓存：无", align="left", height=18)
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

        # Card 5 · advanced tools.
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
        cmds.showWindow(WINDOW_NAME)
        self._apply_panel_theme(
            (workflow_card, connection_card, scene_card, preview_card,
             tools_card, diagnostics_card),
            {self._animation_workflow_button: "tabOn",
             self._model_workflow_button: "tabOff",
             self._connect_button: "primary",
             self._realtime_mode_button: "realtime",
             self._cached_mode_button: "neutral",
             self._light: "disconnected",
             self._capture_button: "primary"})


    def _apply_panel_theme(self, cards, roles):
        """Style only this Maya window's Qt widgets; keep cmds as state owner."""
        if omui is None:  # Pure-Python controller tests have no Maya UI.
            return
        from PySide2 import QtCore, QtGui, QtWidgets
        from shiboken2 import wrapInstance

        window = wrapInstance(int(omui.MQtUtil.findWindow(WINDOW_NAME)),
                              QtWidgets.QWidget)
        window.setFont(QtGui.QFont("Microsoft YaHei UI", 10))
        window.setStyleSheet(PANEL_STYLE)
        class _CardDecoration(QtCore.QObject):
            def __init__(self, card_widget, border, title_widget):
                super(_CardDecoration, self).__init__(card_widget)
                self.border = border
                self.title = title_widget
                card_widget.installEventFilter(self)

            def eventFilter(self, watched, event):
                if event.type() == QtCore.QEvent.Resize:
                    self.border.setGeometry(watched.rect())
                    if self.title is not None:
                        self.title.setFixedWidth(watched.width() - 2)
                return False

        for index, card in enumerate(cards):
            ptr = omui.MQtUtil.findLayout(card)
            widget = wrapInstance(int(ptr), QtWidgets.QWidget)
            children = [child for child in widget.children()
                        if isinstance(child, QtWidgets.QWidget)]
            body = children[-1]
            title = None
            if index:
                # Maya paints its frame header after the Qt style, so the
                # header surface comes from an inert overlay label.
                title = QtWidgets.QLabel(
                    cmds.frameLayout(card, query=True, label=True), widget)
                title.setGeometry(1, 1, widget.width() - 2, 25)
                title.setContentsMargins(10, 0, 0, 0)
                title.setFont(QtGui.QFont("Microsoft YaHei UI", 10, QtGui.QFont.Bold))
                title.setStyleSheet(
                    "background: #393f43; color: #e5e8e9; border: none;"
                    "border-top-left-radius: 3px; border-top-right-radius: 3px;")
                title.setAttribute(QtCore.Qt.WA_TransparentForMouseEvents)
                title.show()
            outline = QtWidgets.QFrame(widget)
            outline.setGeometry(widget.rect())
            outline.setStyleSheet(
                "background: transparent; border: 1px solid #454b4e; border-radius: 4px;")
            outline.setAttribute(QtCore.Qt.WA_TransparentForMouseEvents)
            outline.show()
            outline.raise_()
            self._card_decorations.append(_CardDecoration(widget, outline, title))
            if index == 2:
                divider = QtWidgets.QFrame(body)
                divider.setGeometry(CARD_MARGIN + CARD_HALF + CARD_GAP // 2,
                                    CARD_MARGIN + 4, 1, 58)
                divider.setStyleSheet("background: #656b6e;")
                divider.show()
        for control, role in roles.items():
            ptr = omui.MQtUtil.findControl(control)
            widget = wrapInstance(int(ptr), QtWidgets.QWidget)
            self._styled_controls[control] = widget
            widget.setProperty("mtouRole", role)
        window.style().unpolish(window)
        window.style().polish(window)
        for widget in self._styled_controls.values():
            widget.style().unpolish(widget)
            widget.style().polish(widget)

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
        self._set_enabled(self._playback_cap_menu, realtime and not can_cancel)
        self._set_enabled(self._capture_button, not realtime and can_capture)
        self._set_enabled(self._upload_button, not realtime and can_upload)
        self._set_enabled(self._cancel_capture_button, not realtime and can_cancel)
        self._set_enabled(self._realtime_mode_button, realtime or can_leave)
        self._set_enabled(self._cached_mode_button, connected and not can_cancel)
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
            ("等待捕获完成，或取消捕获。" if can_cancel else "捕获并上传范围，在 UE 开始播放。"))
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
        if summary is None:
            self._set_text(self._cache_text, "缓存：无")
            return
        start, end = summary.capture_range
        capture_time = time.strftime(
            "%Y-%m-%d %H:%M:%S", time.localtime(summary.capture_time))
        self._set_text(
            self._cache_text,
            "缓存：{0}-{1}，{2} 帧，{3:g} fps，捕获于 {4}".format(
                start, end, summary.frame_count, summary.scene_fps, capture_time))

    def _ensure_cached_playback(self):
        if self._workflow != WORKFLOW_ANIMATION or self._session is None \
                or self._scene is None or self._cached_playback is None:
            raise CachedPlaybackError(
                "CACHED_PLAYBACK_NOT_READY",
                "请先完成角色设置并连接 Unreal。")
        return self._cached_playback

    def _on_mode_changed(self, mode):
        if mode == CACHED_MODE and self._workflow != WORKFLOW_ANIMATION:
            return
        if mode == CACHED_MODE:
            if not self._session_ready():
                self._mode = REALTIME_MODE
                self._update_mode_selection()
                self._update_mode_controls()
                self._show_error(make_diagnostic(
                    "CACHED_PLAYBACK_NOT_READY",
                    "请先完成角色设置并连接 Unreal。"))
                return
            try:
                self._ensure_cached_playback().enter()
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
            self._set_text(self._fps_text, "帧率：" + format_fps(fps))
            return fps
        except ValueError:
            unit = cmds.currentUnit(query=True, time=True)
            self._set_text(self._fps_text, "帧率：{0}（不受支持）".format(unit))
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
        self._set_text(self._root_text, "角色根骨骼：" + snapshot.root)
        self._set_text(self._outfit_text, "当前衣服：" + snapshot.outfit)
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
                self._set_text(self._root_text, "角色根骨骼：" + root)
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
                cmds.evalDeferred(lambda: self._show_error(diagnostic, session=True))

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
        self._last_warning = {"missing_in_unreal": [], "missing_in_maya": [],
                              "bone_name_remaps": [],
                              "has_warning": False}
        self._shown_warning_signatures = set()
        self._set_connected(False, "正在连接 Unreal…")
        holder = {}

        def on_event(event):
            self._on_streaming_session_event(holder.get("session"), event)

        try:
            session = StreamingSession.start(
                self._scene, fps, on_event, playback_cap=self._playback_cap,
                workflow=self._workflow,
                blendshapes_enabled=self._blendshapes_enabled)
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

    def _on_streaming_session_event(self, session, event):
        if session is None or self._session is not session:
            return
        if event.kind == "ready":
            if self._workflow == WORKFLOW_ANIMATION:
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
            warning = event.warning or {
                "missing_in_unreal": [], "missing_in_maya": [],
                "bone_name_remaps": [], "has_warning": False}
            self._last_warning = warning
            if warning["has_warning"]:
                self._set_connected(True, "已连接（有警告）：UE 显示实时姿势，请检查差异")
            else:
                self._set_connected(True, "已连接：UE 显示实时姿势")
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
        self._mode = REALTIME_MODE
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
        return "\n\n".join(sections)

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
            label="复制详情", command=lambda *_: self._copy_text(
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

    def _on_maya_exiting(self, *unused):
        del unused
        self._discard_cached_playback()

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

    def close(self, *unused):
        self._remove_maya_exit_callback()
        self._clear_role()

CONTROLLER = None

WINDOW_NAME = "MtoULiveLinkWindow"

DIAGNOSTIC_WINDOW_NAME = "MtoULiveLinkDiagnosticWindow"
