"""Shared Maya layout, theme and control helpers for the MtoU_LiveLink windows.

The panel controller and the two detail windows build their cards, grid rows,
status colours and tooltips through these helpers so every window keeps one
look and one way of talking to ``cmds``. ``cmds`` stays the state owner: the
helpers only render what the caller already decided.

The Maya bindings are read through ``mayaapi`` at call time, never bound at
import time, so every test that patches one binding sees the same replacement
here as in the rest of the adapter.
"""

from .. import mayaapi
from .style import BUTTON_HEIGHT, CARD_GAP, CARD_HALF, CARD_MARGIN, GRID_GAP, PANEL_STYLE


def text_units(text):
    return sum(2 if ord(char) >= 0x2E80 else 1 for char in text)


def elide_text(text, units, middle=False):
    """Shorten ``text`` to ``units`` (CJK counts two) with one ellipsis."""
    if text_units(text) <= units:
        return text
    budget = units - text_units("…")
    if not middle:
        kept = ""
        for char in text:
            if text_units(kept + char) > budget:
                break
            kept += char
        return kept + "…"
    head, tail = "", ""
    chars = list(text)
    while chars:
        char = chars.pop(0)
        if text_units(head + char + tail) > budget:
            break
        head += char
        if chars:
            char = chars.pop()
            if text_units(head + char + tail) > budget:
                break
            tail = char + tail
    return head + "…" + tail


def grid_row(controls, spans=None, height=None, top=0):
    """Attach controls of the current formLayout to equal grid columns.

    ``spans`` gives each control's column count (default one each). Every row
    shares the same columns and the GRID_GAP gutter, so edges line up. ``top``
    insets the row from the top of its own height.
    """
    cmds = mayaapi.cmds
    if height is None:
        height = BUTTON_HEIGHT
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


def control_exists(control):
    cmds = mayaapi.cmds
    if not control or cmds is None:
        return False
    try:
        return bool(cmds.control(control, exists=True))
    except (AttributeError, RuntimeError, TypeError):
        return False


def set_text(control, text, tooltip=None):
    """Render one label; the annotation mirrors it unless ``tooltip`` is given."""
    cmds = mayaapi.cmds
    if control and control_exists(control):
        cmds.text(control, edit=True, label=text,
                  annotation=text if tooltip is None else tooltip)


def set_tooltip(control, tooltip):
    cmds = mayaapi.cmds
    if control_exists(control):
        try:
            cmds.control(control, edit=True, annotation=tooltip or "")
        except (AttributeError, RuntimeError, TypeError):
            pass


def set_enabled(control, enabled):
    cmds = mayaapi.cmds
    if control_exists(control):
        try:
            cmds.control(control, edit=True, enable=bool(enabled))
        except (AttributeError, RuntimeError, TypeError):
            pass


def set_visible(control, visible):
    cmds = mayaapi.cmds
    if control_exists(control):
        try:
            cmds.control(control, edit=True, visible=bool(visible),
                         manage=bool(visible))
        except (AttributeError, RuntimeError, TypeError):
            pass


def apply_panel_theme(window_name, cards, roles, divider_card=None):
    """Style only this Maya window's Qt widgets; keep cmds as state owner.

    Every card with a visible label gets the header overlay; only
    ``divider_card`` gets the two-column divider. Returns the card decorations
    and the styled controls so the caller keeps them alive for the window's
    lifetime.
    """
    omui = mayaapi.omui
    cmds = mayaapi.cmds
    if omui is None:  # Pure-Python controller tests have no Maya UI.
        return [], {}
    from PySide2 import QtCore, QtGui, QtWidgets
    from shiboken2 import wrapInstance

    window = wrapInstance(int(omui.MQtUtil.findWindow(window_name)),
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

    decorations = []
    for card in cards:
        ptr = omui.MQtUtil.findLayout(card)
        widget = wrapInstance(int(ptr), QtWidgets.QWidget)
        children = [child for child in widget.children()
                    if isinstance(child, QtWidgets.QWidget)]
        body = children[-1]
        title = None
        if cmds.frameLayout(card, query=True, labelVisible=True):
            # Maya paints its frame header after the Qt style, so the header
            # surface comes from an inert overlay label.
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
        decorations.append(_CardDecoration(widget, outline, title))
        if card == divider_card:
            divider = QtWidgets.QFrame(body)
            divider.setGeometry(CARD_MARGIN + CARD_HALF + CARD_GAP // 2,
                                CARD_MARGIN + 4, 1, 58)
            divider.setStyleSheet("background: #656b6e;")
            divider.show()
    styled_controls = {}
    for control, role in roles.items():
        ptr = omui.MQtUtil.findControl(control)
        widget = wrapInstance(int(ptr), QtWidgets.QWidget)
        styled_controls[control] = widget
        widget.setProperty("mtouRole", role)
    window.style().unpolish(window)
    window.style().polish(window)
    for widget in styled_controls.values():
        widget.style().unpolish(widget)
        widget.style().polish(widget)
    return decorations, styled_controls


def release_panel_theme(decorations):
    """Detach and delete card decorations while Python is still running.

    A decoration is a Python object Qt calls back into; one still installed
    when Maya tears down its windows at exit would call into a finished
    interpreter. Decorations Qt already deleted with their window are skipped,
    so releasing twice is safe.
    """
    if not decorations or mayaapi.omui is None:
        return
    import shiboken2

    for decoration in decorations:
        if not shiboken2.isValid(decoration):
            continue
        watched = decoration.parent()
        if watched is not None:
            watched.removeEventFilter(decoration)
        shiboken2.delete(decoration)
