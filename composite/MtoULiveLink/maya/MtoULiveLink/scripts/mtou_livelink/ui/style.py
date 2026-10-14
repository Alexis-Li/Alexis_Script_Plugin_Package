"""Shared panel styling constants."""


TOGGLE_ON_BACKGROUND = (0.16, 0.55, 0.24)

TOGGLE_OFF_BACKGROUND = (0.26, 0.26, 0.26)

TAB_ON_BACKGROUND = (0.16, 0.42, 0.56)

TAB_OFF_BACKGROUND = (0.18, 0.19, 0.20)

LIGHT_ON_BACKGROUND = (0.09, 0.40, 0.23)

LIGHT_OFF_BACKGROUND = (0.34, 0.10, 0.11)

# Panel grid: card stack width, outer margin, and the shared column grid.
# Maya adds a one-pixel control frame around each Qt-backed control.
PANEL_WIDTH = 432

PANEL_MARGIN = 8

CARD_MARGIN = 8

GRID_GAP = 8

CARD_GAP = GRID_GAP

ROW_SPACING = 9

TAB_HEIGHT = 32

BUTTON_HEIGHT = 34

CARD_CONTENT = PANEL_WIDTH

CARD_HALF = (CARD_CONTENT - CARD_GAP) // 2

PRIMARY_BACKGROUND = (0.16, 0.42, 0.56)

PANEL_STYLE = """
QWidget#MtoULiveLinkWindow { background: #2d3133; color: #e5e8e9; }
QPushButton {
    background: #474c4f; color: #f2f3f3; border: 1px solid #62686a;
    border-radius: 3px; padding: 0 4px;
}
QPushButton:hover:enabled { background: #535b5e; border-color: #8a969a; }
QPushButton:pressed:enabled { background: #343a3d; }
QPushButton:disabled { background: #363a3d; color: #a3a8aa; border-color: #484d50; }
QPushButton[mtouRole="tabOn"], QPushButton[mtouRole="primary"] {
    background: #286f91; border-color: #499aba; color: #ffffff; font-weight: bold;
}
QPushButton[mtouRole="tabOn"]:hover:enabled,
QPushButton[mtouRole="primary"]:hover:enabled { background: #3583a6; }
QPushButton[mtouRole="tabOff"] { background: #2e3234; border-color: #454b4e; }
QPushButton[mtouRole="realtime"] {
    background: #198d54; border-color: #31af6c; color: #f9fffc; font-weight: bold;
}
QPushButton[mtouRole="realtime"]:hover:enabled { background: #23a464; }
QPushButton[mtouRole="primary"]:disabled {
    background: #363a3d; color: #a3a8aa; border-color: #484d50;
}
QLabel { color: #e5e8e9; }
QLabel[mtouRole="disconnected"] {
    background: #4b1e20; border: 1px solid #9d3d43; border-radius: 3px;
    color: #ff727a; font-weight: bold;
}
QLabel[mtouRole="connected"] {
    background: #1b4930; border: 1px solid #319965; border-radius: 3px;
    color: #86e5ac; font-weight: bold;
}
QComboBox {
    background: #393e41; color: #e5e8e9; border: 1px solid #60686b;
    border-radius: 3px; padding-left: 5px;
}
QComboBox:disabled { color: #a3a8aa; border-color: #484d50; }
QCheckBox { color: #e0e3e4; }
"""
