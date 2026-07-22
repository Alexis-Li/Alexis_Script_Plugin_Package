# ZiSpread

[English](README.md)

## 简介

`ZiSpread_Rebuilt.py` 是 ZiSpread 插件的 Python 重构版，原版是 64 位 C++
插件。Python 版不需要针对不同 Maya 版本分别加载 `ZiSpread_20XX.mll`，并且兼容 Python 2 和 Python 3。

## 安装方式

无需安装插件。在 Maya 脚本编辑器的 Python 页运行 `ZiSpread_Rebuilt.py`
的全部内容即可，也可以把完整脚本保存到工具架按钮中。

## 使用方式

1. 选择需要均匀分布的多条环线。
2. 运行脚本。
3. 在视口中按住鼠标左键或中键横向拖动。
4. 松开鼠标，确认结果。
5. 按 Ctrl+Z 撤销操作。
