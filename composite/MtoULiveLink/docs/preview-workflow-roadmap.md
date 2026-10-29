# MtoU 预览工作流范围与后续整合

2026-10-13：[母规格 #47](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/47)
的四个实现任务和三个有界验证均已通过各自验收。母项汇总结论、适用基线和
证据边界见[母规格验收记录](../../../docs/project-history/mtou-livelink/issue-47-preview-workflow-acceptance.md)。

当前正式组件为 0.10.0（未发布）、协议 v11；I-1／I-2／I-3 均已完成，位于
`codex/mtou-preview-workflow`；下文 0.9.0／v9 为本文档记录的历史验收基线。
`composite-mtou-livelink-v0.5.0` 保留为项目负责人接受的历史稳定里程碑；
本轮验收不创建新发布标签、正式安装包或宿主版本认证。

## 已完成的工作流

| 范围 | 当前行为 | 验收依据 |
| --- | --- | --- |
| #48 双端界面 | Maya 分区操作；UE Details 区分预览／连接状态，显示角色组成、可用动作、中文摘要和完整诊断，错误优先级与中断状态来自真实生命周期。 | [界面验收](../../../docs/project-history/mtou-livelink/issue-48-ue-details-acceptance.md) |
| #49 捕获与基础回放 | Maya 捕获、取消、上传；自定义范围关闭时取 Playback Range，开启时包含两端；开始时冻结范围／fps。UE 完整验证后 Ready，用户主动播放／停止／再次播放。 | [缓存工作流验收](../../../docs/project-history/mtou-livelink/issue-49-cache-workflow-acceptance.md) |
| #50 交互回放 | UE 暂停／继续、按已采样 Maya 源帧定位、循环；暂停不算性能超时，定位进入暂停，播放段与完整缓存的完成语义不同。 | [交互回放验收](../../../docs/project-history/mtou-livelink/issue-50-interactive-playback-acceptance.md)、[ADR 0013](adr/0013-upload-the-cache-before-local-replay.md) |
| #51 服装比较 | Ready Generated Preview 与原始服装反复切换，保留姿势、会话、Preview、身体和部件；两种显示使用同一协商 Morph 集合。 | [比较验收](../../../docs/project-history/mtou-livelink/issue-51-garment-comparison-acceptance.md)、[ADR 0011](adr/0011-activate-only-current-outfit-morphs.md) |

缓存停止／自然完成保留缓存和最后姿势，返回实时是明确操作。单次顺序播放
继续不静默丢帧，旧会话和旧命令不能操作新缓存。服装比较只改变显示；真实输入
变化仍遵守 [ADR 0002](adr/0002-end-streaming-when-preview-inputs-change.md)。
自然编辑器 tick、部件 Morph 和真实 C01 服装视觉比较均已纳入 #51 的最终验收。

## 已完成的有界验证

| 原型 | 已证明的能力 | 保留边界 |
| --- | --- | --- |
| #52 UE 相机与 Sequencer | 已求值透视相机及参数／门、cuts、一层子序列、不同 fps／非零起始／子帧、求值身份与实际回读、自然 UE 编辑器跟随和失败恢复。 | 独立原型协议 v2；单关节 witness 未接产品全角色流。交互 Maya 回调、持续播放与缓存时钟接入仍需后续验证；不声称跨渲染器 DOF 图像等价。 |
| #53 UE 场景参考 | 官方 FBX 交接显式关卡范围，保持世界摆放和实例；分别检查交付文件与 FBX 图片数据；灰模、暂存替换、回滚与恢复。 | 默认 `(x,z,y)`，相机原型为 `(y,z,-x)`，原型的显式换算不等于产品统一。Landscape、Nanite、WP 仅按实测样例和完整性报告；不保证生产规模、真实崩溃或任意嵌套制作节点。 |
| #54 独立道具／多角色 | 双 Subject 独立根／目标配对、必要骨骼严格协商、共同帧、同名隔离、一次共同摆放、独立移除及最小 Sequencer 接管／恢复。真实输入为 C01 动画＋C02，合成 full-body／arms 另测。 | 不改产品 v9／单 Subject；移动 socket 拒绝，任意生产 Sequence 和原生产 Anim Blueprint 行为未获认证。缓存只有设计。 |

各项可复现输入、来源、测量和问题闭环分别见
[#52 验收](../../../docs/project-history/mtou-livelink/issue-52-camera-sync-acceptance.md)、
[#53 验收](../../../docs/project-history/mtou-livelink/issue-53-scene-reference-acceptance.md)、
[#54 验收](../../../docs/project-history/mtou-livelink/issue-54-multi-subject-acceptance.md)。
三者的原型使用入口仍位于各自 `prototypes/` 目录。

## 正式接入与联合验收

[#56](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/56) 的 I-1（#57）、
I-2（#58）、I-3（#59）均已审核并集成。正式入口支持参考准备、显式配对、
单／双对象实时与镜头联动、返回实时和结束预览，保留单角色缓存与 Model 工作流。
共同空间、互斥时间控制、整组显示和恢复行为的 INT-01–14 结果见
[联合验收记录](../../../docs/project-history/mtou-livelink/preview-workflow-integration-acceptance.md)。
该记录也说明后续 #91 失败刷新修复的验证范围，以及 Topia、Maya 2022.4 和宿主测量的限制。

[接入设计](preview-workflow-integration-plan.md)保留设计依据；当前协议为
[v11](../protocol/v11.md)。合入 `main`、确定稳定里程碑和发布是独立的下一阶段。
多对象缓存、Sequencer 驱动缓存与更广内容支持需要另行定义范围。

## 持续约束

最终效果以 UE 场景和镜头为准，Maya 制作动画；沿用既有导出、导入与 Sequencer
组装。独立道具和不同骨架角色不套用 Additional Parts，也不自动重定向。
场景参考范围必须显式，不能悄悄改成 Actor 选择；不交付图片数据，允许材质
分配及外部图片路径引用。共同原点／摆放必须同时适用于相机、参考和动画对象。

本阶段不扩展到刷新性能优化、固定检查姿势／书签、问题区域可视化、配置记忆、
自动恢复／自动重连、新动画交付流程、录制 UE 动画、PIE／打包运行或跨机器。
后续整合的持续播放测量是新联动路径的验收，不是已有 Preview 刷新优化。
