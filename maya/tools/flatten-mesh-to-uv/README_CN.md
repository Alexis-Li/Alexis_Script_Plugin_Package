# Flatten Mesh To UV

[English](README.md)

## 简介

Flatten Mesh To UV 会把所选模型按照当前 UV Set 的展开结果，生成一个位于 XY
平面的新多边形模型。UV 接缝会被拆开，原模型拓扑关系和 UV 坐标会保留。

适合将 UV 展开结果转换成真实几何体，制作 UV 线框、裁片、贴花和布料 2D
参考，或导出为可继续建模的平面网格。

脚本兼容 Python 2 和 Python 3，作者已在 Maya 2020、2022、2024 中测试。

## 安装方式

1. 将 `package/FlattenMeshToUV/plug-ins/FlattenMeshToUV.py` 复制到
   `Documents\maya\20xx\plug-ins`；没有 `plug-ins` 文件夹时可自行创建。
2. 在 Maya 的 Plug-in Manager（插件管理器）中加载 `FlattenMeshToUV.py`，建议勾选 Loaded 和 Auto load。

## 使用方式

1. 在 Maya 中选择一个多边形模型。
2. 确认模型当前 UV Set 是需要转换的 UV Set。
3. 在脚本编辑器的 Python 页运行
   `package/FlattenMeshToUV/scripts/FlattenMeshToUV_Start.py` 中的内容。
4. 经常使用时，可把启动代码保存到工具架或自用工具箱中。
