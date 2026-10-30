# NitroPoly

[English](README.md)

## 工具简介

NitroPoly 4.0.4 是 Maya 多边形建模工具集，提供选择、拓扑、轴心、连接和循环边
等常用功能。

## 支持版本

兼容 Python 2 和 Python 3；作者已在 Maya 2020、2022、2024 中测试。

## 安装方式

将 `scripts/NitroPoly.py` 复制到 Maya 用户目录下的 `scripts` 文件夹。

## 使用方式

将以下 Python 命令保存为 Maya 工具架按钮：

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
`NitroPoly.py` 并执行完整文件。
