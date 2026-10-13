# 预览联动能力调研

调研基线：2026-09-22。本文保留最初的官方能力来源与选型依据；
实际验证结论由以下原型记录维护，产品接入由
[整合方案](preview-workflow-integration-plan.md)承接。

| 主题 | 后续验证与契约 |
| --- | --- |
| UE 相机与 Sequencer → Maya | [官方能力核对](../prototypes/camera-sync/official-capabilities.md)、[相机映射](../prototypes/camera-sync/mapping.md)、[#52 验收](../../../docs/project-history/mtou-livelink/issue-52-camera-sync-acceptance.md) |
| UE 关卡参考几何 → Maya | [官方能力核对](../prototypes/scene-reference/official-capabilities.md)、[交接契约](../prototypes/scene-reference/transfer.md)、[#53 验收](../../../docs/project-history/mtou-livelink/issue-53-scene-reference-acceptance.md) |
| 独立道具与多角色 | [对象协议](../prototypes/multi-subject/protocol.md)、[#54 验收](../../../docs/project-history/mtou-livelink/issue-54-multi-subject-acceptance.md) |

## 相机与时间同步的来源

Epic 的 [Maya Live Link 教程](https://dev.epicgames.com/documentation/unreal-engine/live-link-stream-maya-to-unreal-engine?application_version=5.3)
描述 Maya → UE 的相机跟随。Autodesk 的
[Sync Time](https://help.autodesk.com/cloudhelp/2024/ENU/UnrealLiveLink/files/UnrealLiveLink_unreal_livelink_editor_html.html)
和 [Asset Linking](https://help.autodesk.com/cloudhelp/2023/ENU/UnrealLiveLink/files/UnrealLiveLink_streaming_animation_html.html)
提供时间通知、帧率处理和 Sequence 关联参考，但后者也包含向 UE 动画资产写入的流程。

[Autodesk 功能说明](https://marketplace.autodesk.com/apps/2408a020-87ce-4682-b5cc-1e4b9506d21b)
和[公开源码](https://github.com/Autodesk/LiveLink)是属性映射与实现参考。
这次文档调研没有验证其在 Maya 2024／UE 5.7.4 或 Topia 的编译运行兼容性。

镜头映射同时涉及
[Cine Camera](https://dev.epicgames.com/documentation/unreal-engine/cinematic-cameras-in-unreal-engine)
的 Filmback、Lens、Focus、Crop 和独立的
[输出分辨率](https://dev.epicgames.com/documentation/unreal-engine/BlueprintAPI/MovieRenderPipeline/GetDesiredOutputResolution)。
参数、投影构图和最终景深图像是不同的验证对象；具体可用范围见相机原型记录。

## 参考几何的来源

[ULevelExporterFBX](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/ULevelExporterFBX)、
[AssetExportTask](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/AssetExportTask?application_version=5.7)
和 [FbxExportOption](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/FbxExportOption?application_version=5.7)
构成官方关卡导出候选。是否保留摆放、覆盖指定关卡范围、输出图片数据及支持特定
对象类别，需要检查实际产物；这些结论已归入场景参考原型的能力与验收记录。
