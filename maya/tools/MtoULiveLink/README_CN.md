# MtoULiveLink

[English](README.md)

## 工具简介

MtoULiveLink 将一套 Maya 已求值变形骨架及其匹配的 BlendShape 流式发送到
本机 Unreal 的 `MtoU_Character` Live Link 主题，无需修改或导出场景。

## 支持版本

- 64 位 Windows
- 仅支持 Autodesk Maya 2022.4

## 安装方式

将 `scripts/MtoULiveLink.py` 复制到 Maya 脚本目录，或直接在 Maya Python
脚本编辑器中打开该文件。

## 使用方式

1. 先在 Unreal 中放置并配置绑定 Actor。
2. 在 Maya 中只选择一个变形根骨骼。
3. 执行 `scripts/MtoULiveLink.py`。
4. 点击 **Connect**。
5. 在 Maya 中调整姿势、播放或拖动时间轴。
6. 完成后点击 **Disconnect**。
7. 修改拓扑或任一宿主重启后，重新连接。
