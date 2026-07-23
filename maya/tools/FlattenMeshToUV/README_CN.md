# Flatten Mesh To UV

[English](README.md)

## 工具简介

Flatten Mesh To UV 会根据所选模型当前 UV Set 的展开结果，在 XY 平面生成一个
新的多边形模型。工具会拆开 UV 接缝，并保留拓扑关系和 UV 坐标。

## 支持版本

兼容 Python 2 和 Python 3；作者已在 Maya 2020、2022、2024 中测试。

## 安装方式

1. 将 `plug-ins/FlattenMeshToUV.py` 复制到 Maya 的 `plug-ins` 目录。
2. 在 Maya 插件管理器中加载 `FlattenMeshToUV.py`；可按需启用自动加载。

## 使用方式

1. 选择一个多边形模型，并激活需要转换的 UV Set。
2. 在 Maya Python 脚本编辑器中运行 `scripts/RunFlattenMeshToUV.py`。
3. 经常使用时，可将启动代码保存到工具架按钮。
