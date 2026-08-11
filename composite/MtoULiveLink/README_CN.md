# MtoU_LiveLink

[English](README.md)

## 简介

MtoU_LiveLink 是一个由 Maya 与 Unreal 组件组成的复合插件，用于在 Unreal
Live Link 中本地预览一套已求值的 Maya 变形骨架及其匹配的 BlendShape。两个组件
通过本机回环连接协作，同时保持各自可独立安装和打包。

## 支持版本

- 64 位 Windows
- Autodesk Maya 2022.4
- 官方原版 Unreal Editor 5.7.4

不声明兼容第三方修改版 Unreal Engine 5.7。

## 安装

将对应组件安装到各宿主软件：

1. Maya：将 `maya/MtoULiveLink/scripts/MtoULiveLink.py` 复制到 Maya 脚本目录，
   或直接在 Maya Python 脚本编辑器中执行。
2. Unreal：将完整的 `unreal/MtoULiveLink/` 目录复制到
   `<Project>/Plugins/MtoULiveLink/`。安装后的描述文件必须位于
   `<Project>/Plugins/MtoULiveLink/MtoULiveLink.uplugin`。
3. 编译 Unreal 项目，启用 **Live Link** 和 **MtoU_LiveLink**，然后重启编辑器。

## 使用

1. 在 Unreal 的目标内容浏览器目录中创建 **MtoU_LiveLink Binding**，指定
   **Skeletal Mesh**，并将该绑定拖入关卡。
2. 在 Maya 中只选择一个变形根骨骼，运行 `MtoULiveLink.py`，然后点击
   **Connect**。
3. 在 Maya 中调整姿势、播放或拖动时间轴，即可驱动 `MtoU_Character` Live Link
   主题。完成后点击 **Disconnect**；修改拓扑或任一宿主重启后需要重新连接。

接收端会保留每个已放置 Actor 的变换，且不会创建 Animation Sequence。多个蒙皮
网格部件上的同名 BlendShape 在求值一致时会合并为一条 Unreal 曲线发送；若数值
不同，采样会停止并列出所有冲突的 Maya 插口。
