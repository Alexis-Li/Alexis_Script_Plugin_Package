# NitroPoly

[English](README.md)

## 工具简介

NitroPoly 4.0.4 是 Maya 多边形建模工具集，提供选择、拓扑、轴心、连接和循环边
等常用功能。

## 支持版本

兼容 Python 2 和 Python 3；作者已在 Maya 2020、2022、2024 中测试。

## 安装方式

运行文件只有 `NitroPoly.py`。将它复制到 Maya 用户目录下的 `scripts` 文件夹，
不要复制或创建单独的 `NitroPolyStart.py`。

## 使用方式

把以下 Python 命令直接保存为 Maya 工具架按钮的内容。它只是按钮命令，不是
另一个脚本文件：

```python
try:
    reload
except NameError:
    from importlib import reload

import NitroPoly
reload(NitroPoly)
NitroPoly.main()
```

如果只想临时运行而不安装，也可以在 Maya 脚本编辑器的 Python 选项卡中打开
`NitroPoly.py` 并执行完整文件。这是另一种独立用法，不需要先复制到 `scripts`
目录，也不需要创建工具架按钮；两种流程不叠加使用。
