# MtoULiveLink

[English](README.md)

## 简介

MtoULiveLink 将本机的一名 Maya 角色作为原生 Live Link 动画和曲线接收，
同时保留每个已放置 Actor 的变换。

## 支持版本

- 64 位 Windows
- 官方原版 Unreal Editor 5.7.4

目前不声明兼容第三方 Unreal Engine 5.7 版本。

## 安装方式

1. 将完整的 `MtoULiveLink` 目录复制到 `<Project>/Plugins/MtoULiveLink/`，并确认
   描述文件位于 `<Project>/Plugins/MtoULiveLink/MtoULiveLink.uplugin`。
2. 编译项目。
3. 启用 **Live Link** 和 **MtoU_LiveLink**。
4. 重启 Unreal Editor。

## 使用方式

1. 在所选内容浏览器文件夹中创建 **MtoU_LiveLink Binding**。
2. 将其 **Skeletal Mesh** 设置为现有资产。
3. 将该绑定拖入关卡。
4. 从 Maya 连接。

此流程不会创建 Animation Sequence。
