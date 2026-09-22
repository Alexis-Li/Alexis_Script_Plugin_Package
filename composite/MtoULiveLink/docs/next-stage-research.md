# 下一阶段官方能力调研

调研日期：2026-09-22。本文为后续设计依据，不表示这些能力已经进入
MtoU；具体范围见 [预览工作流路线图](preview-workflow-roadmap.md)。

## 相机与 Sequencer

- Epic 的 Live Link 框架与 Autodesk 发布的 **Unreal Live Link for Maya**
  需要区分。Epic 教程中的 `Enable Camera Sync` 明确是让 UE 相机跟随
  Maya 视口相机；不能据此认定它支持本项目需要的 UE → Maya。
  [Epic 相机同步教程](https://dev.epicgames.com/documentation/unreal-engine/live-link-stream-maya-to-unreal-engine?application_version=5.3)
- Autodesk 插件的 `Sync Time` 明确支持 Maya Time Slider 与 UE 播放头
  双向同步。其 Asset Linking 文档也包含连接 Level Sequence 的工作流，
  但同时会把 Maya 动画推入 UE 资产。MtoU 可参考时间通知和帧率处理，
  无需引入整套动画资产写入流程。
  [Sync Time](https://help.autodesk.com/cloudhelp/2024/ENU/UnrealLiveLink/files/UnrealLiveLink_unreal_livelink_editor_html.html)、
  [Asset Linking](https://help.autodesk.com/cloudhelp/2023/ENU/UnrealLiveLink/files/UnrealLiveLink_streaming_animation_html.html)
- Autodesk 公开功能包括相机变换、视角、焦距、film gate、camera aperture、
  film aspect ratio、景深、对焦距离和 f-stop；这些是 Maya → UE 的能力，
  可作为属性映射参考。公开源码可供评估复用，但本次没有证据证明
  UE → Maya 全参数相机同步已经可以直接启用。
  [官方功能说明](https://marketplace.autodesk.com/apps/2408a020-87ce-4682-b5cc-1e4b9506d21b)、
  [Autodesk 源码](https://github.com/Autodesk/LiveLink)

**设计建议**：相机同步与 Sequencer 可以同一期推进，分别负责镜头数据、
当前 Camera Cut 和时间控制。UE 是镜头权威来源；时间控制则应明确一次
操作只有一个驱动方，避免 UE 拖帧 → Maya 求值 → UE 回传又改时间的循环。
这些是 MtoU 的设计建议，不是官方插件已经满足的保证。

相机验收应覆盖世界变换、焦距、传感器尺寸、水平/垂直视角、焦点距离、
f-stop、景深开关、画幅、裁切和 Maya 分辨率门。UE Cine Camera 的 Filmback、
Lens、Focus、Crop 是不同设置，输出分辨率又属于渲染配置；因此只同步
一个焦距或 FOV 不足以保证构图一致。需要明确使用哪一个输出配置，并对
宽高比、裁切和 overscan 做成对验证。
[Cine Camera](https://dev.epicgames.com/documentation/unreal-engine/cinematic-cameras-in-unreal-engine)、
[输出分辨率 API](https://dev.epicgames.com/documentation/unreal-engine/BlueprintAPI/MovieRenderPipeline/GetDesiredOutputResolution)

参数一致、投影构图一致、景深最终图像一致应分别验证。上述资料没有承诺
Maya 与 UE 渲染器的散景和后处理逐像素相同；不应把同步参数写成图像完全
等价的承诺。验收仍以 UE 实际效果为准。

## UE 场景参考几何 → Maya

Epic 已提供 `ULevelExporterFBX`，导出任务也有 `selected` 选项；可以优先
验证复用关卡导出能力，避免自建网格序列化系统。`FbxExportOption` 包含
材质输入烘焙、LOD、碰撞和源网格选项，但公开选项并没有一个能据此直接
保证“完全无贴图”的统一开关。关闭烘焙不等于证明输出中没有贴图引用，
需要检查实际输出并按需加无材质几何适配。
[LevelExporterFBX](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/ULevelExporterFBX)、
[AssetExportTask 5.7](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/AssetExportTask?application_version=5.7)、
[FbxExportOption 5.7](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/FbxExportOption?application_version=5.7)

**建议验证范围**：先验证所选关卡范围内的普通静态网格，保留组装后的世界
位置、旋转和缩放，以轻量灰模进入 Maya；不是只导出 Content Browser 中的
原始资产。相机、角色和场景参考需要共用同一个空间转换，不能各自归零。
蓝图组件、实例化网格、Level Instance、Landscape、Nanite 和未加载的
World Partition 内容需要分别明确支持范围，不能把 API 存在当作全场景
导出正确的证明。用户所说“所选关卡”也应在实现时明确映射到关卡范围，
不能悄悄缩为当前选中的 Actor。

## 道具与多角色

以下为基于项目 [CONTEXT](../CONTEXT.md) 的设计推论：

- 独立绑定的道具与角色分别建立明确的 Maya 根节点、UE 目标和 Subject，
  共享时间与场景放置关系；已有动画导出和 Sequencer 组装流程继续承担交付。
  首先验证一个角色加一个骨骼道具的预览，尤其是附着偏移和根运动是否被
  重复应用。
- 女主全身与仅手臂的不同骨架作为可选择的独立目标，各自完成骨架匹配。
  当前 Additional Part 属于同一个 Character composition，要求共享 Primary
  Driver 的 Skeleton，不能用它强行表示另一个角色或不同骨架。
- 多角色需要多个 Subject 与共同的帧时间；缓存回放应采用共同时间定位，
  避免各角色分别启动计时后逐渐错位。既有
  [缓存先上传再本地播放决策](adr/0013-upload-the-cache-before-local-replay.md)
  仍适用，但未来 seek、loop、Sequencer 时间驱动需要单独扩展其播放语义。

## 证据边界

本次完成官方文档和仓库契约核对，没有安装 Autodesk 插件或开展宿主联调。
公开文档跨多个产品版本，部分 Epic 页面当前默认显示更新版本；这些资料
不能证明 Autodesk 插件在本项目 Maya 2024、UE 5.7.4 / Topia 环境可直接
编译或运行。后续先验证官方可复用代码、宿主 API 与上述最小场景，再决定
适配范围；不从文档宣称完整兼容或全参数图像等价。
