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

1. 在 Unreal 中创建 **MtoU_LiveLink Binding**，指定 **Skeletal Mesh**，
   并确保关卡中只有一个 Binding Actor。
2. 在 Maya 中运行 `MtoULiveLink.py`，选择唯一的变形根骨骼，再点击
   “设置角色”。工具会自动寻找 `Display_ctrl` 和 Clothes 枚举；
   若候选不唯一，请使用手动 Display 按钮。
   若骨架中存在发送后会冲突的重名骨骼，可在错误弹窗或主窗口点击
   “选中重名骨骼”，一次选中所有冲突 joint，再在大纲视图中定位处理。
   重名不会立即阻止连接；UE 会按已匹配父级寻找导入时自动追加数字的
   唯一骨骼名。映射成功时连接并警告，无法唯一映射时才拒绝连接。
3. 确认当前衣服和场景帧率，再点击“连接”。传输会按 Maya 实际
   场景帧率采样，支持 1–60 fps 及小数帧率。
4. 在 Maya 中调整姿势、播放或拖动时间轴。切换 Clothes 枚举后会
   自动断开；请在 UE 中替换新衣服的 Binding Actor，再手动重连。

接收端会保留每个已放置 Actor 的变换，且不会创建 Animation Sequence。Binding
Actor 会在 Unreal 编辑器中持续更新动画；连接期间，插件会临时强制关卡视口进入
“实时”模式，并在断开连接后恢复各视口原先的设置。断开连接会清除最后一个传输帧，
使模型回到参考姿势，而不是继续保留旧姿势。多个蒙皮网格部件上的同名 BlendShape 在求值
一致时会合并为一条 Unreal 曲线发送；若数值不同，采样会停止并列出所有冲突的 Maya
插口。Maya 和 Unreal 单方存在的 BlendShape 会作为不阻断连接的警告显示。协议版本
2 要求 Maya 和 Unreal 两端都安装匹配的 0.2.0 组件。
