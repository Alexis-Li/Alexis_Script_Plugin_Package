# MtoU_LiveLink

[English](README.md)

## 简介

MtoU_LiveLink 是一个由 Maya 与 Unreal 组件组成的复合插件，用于在 Unreal
Live Link 中本地预览一名已求值的 Maya 角色。两个组件通过已记录的本机协议协作，
同时保持各自可独立安装和打包。

## 支持版本

- 64 位 Windows
- Autodesk Maya 2022.4
- 官方原版 Unreal Editor 5.7.4

不声明兼容第三方修改版 Unreal Engine 5.7。

## 安装

仅将对应组件安装到各宿主软件：

1. Maya：使用 [`maya/MtoULiveLink/`](maya/MtoULiveLink/README_CN.md) 下的文件。
   将 `scripts/MtoULiveLink.py` 复制到 Maya 脚本目录，或直接在 Maya Python
   脚本编辑器中执行。
2. Unreal：将完整的
   [`unreal/MtoULiveLink/`](unreal/MtoULiveLink/README_CN.md) 目录复制到
   `<Project>/Plugins/MtoULiveLink/`。安装后的描述文件必须位于
   `<Project>/Plugins/MtoULiveLink/MtoULiveLink.uplugin`。
3. 编译 Unreal 项目，启用 **Live Link** 和 **MtoU_LiveLink**，然后重启编辑器。

## 使用

在 Unreal 中创建 **MtoU_LiveLink Binding**，指定 Skeletal Mesh，并将其拖入关卡。
在 Maya 中只选择一个变形根骨骼，运行 `MtoULiveLink.py`，然后点击 **Connect**。
在 Maya 中调整姿势、播放或拖动时间轴即可驱动 Unreal Live Link；完成后点击
**Disconnect**。
