<!-- issue-report:v1 kind=review issue=47 key=review-01 -->
## 审核结论

- 状态：通过
- 基线：汇总审核 checkout `77da31afe358963413d92ee55c1031235a0037b9`，本地与已发布分支 `codex/mtou-preview-workflow` 一致。主产品最新实现／已审核基线为 `12199b15f8134e533a44e21a62744d22b0cff2b5`；三项原型各自基线见下表。此次只交付验收和方案文档，未提交新的运行时实现。
- 范围：#47 原有四项实现＋三项有界验证的母规格汇总验收，覆盖 User Stories 1–24 及三个原型共同规划的前置条件。支持关闭 #47；不把后续整合、完整多对象缓存、任意生产场景或新宿主认证纳入本次通过。
- 依据：[#47 需求](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/47)、下列七项最终独立审核／当前汇总、各项目验收记录与原型源报告归档。新方案为[后续整合草案](../../../composite/MtoULiveLink/docs/preview-workflow-integration-plan.md)，不是当前实现。
- 阻塞项：无
- 本次验证：混合
- 更新日期：2026-10-13

### 结论

母规格验收通过。七个直接子任务均为 CLOSED，最终证据覆盖各自约定范围；
已知关闭阻塞均在各子项中复验关闭。本轮没有用“已关闭”代替证据检查：复核了
需求、最终审核、历史记录、原型最终 JSON 与当前源码基线，并执行当前纯测试。

#47 明确将相机、场景参考、双对象限定为可运行原型与边界交付，产品接入统一
后置。三个原型现已达到共同规划条件，方案同步准备完成。遗留产品风险被明确
列入下一阶段联合验收；它们没有被误标为已经实现，也不追加为原母规格的关闭条件。

| 母规格范围 | 子项与最终来源 | 通过结论与限制 |
| --- | --- | --- |
| User Stories 1–5：界面、统一状态、可恢复提示 | [#48 最终复验](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/48#issuecomment-5862474540)、[记录](issue-48-ue-details-acceptance.md) | Maya 沿用已接受面板；UE 中断状态、错误优先级、本地化三项修复复验通过。视觉沿用用户反馈和宿主走查；section 1／4 的特定错误由原文自动化覆盖，未补真实歧义截图。 |
| User Stories 6–11、15–16：范围捕获、上传、UE 基础播放与恢复 | [#49 最终验收](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/49#issuecomment-5862801938)、[记录](issue-49-cache-workflow-acceptance.md) | 真实 C01 默认 1–24 和自定义 [-1,-1]，Ready 不自动播放、停止／再播放、重捕获、回到实时；实际骨骼、部件 Morph 和 BaseColor 证据。短范围不构成长缓存性能保证。 |
| User Stories 12–16：暂停、循环、源帧定位与有效性 | [#50 最终复验](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/50#issuecomment-5871765100)、[记录](issue-50-interactive-playback-acceptance.md) | 真实 Maya peer 交互、120 秒模拟暂停、循环／端点／旧身份隔离及严格 int32 输入验证通过。使用合成角色，不声称本项生产场景或物理 Slate 点击验收。 |
| User Stories 17–18：服装比较与 Morph／身体部件保留 | [#51 最终验收](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/51#issuecomment-5865635414)、[记录](issue-51-garment-comparison-acceptance.md) | 自然 editor tick 覆盖 Generated／Driver-only／part-only Morph；真实 C01 三姿势、六状态、五次切换及 BaseColor 比较通过。未把早期开发自测当最终验收。 |
| User Stories 19–20：相机、Sequencer 有界验证 | [#52 当前汇总](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/52#issuecomment-5888803268)、[记录](issue-52-camera-sync-acceptance.md) | 透视镜头／门、cuts／子帧／求值身份、回读与恢复通过。review-02 原评论 6055097768 见源归档；完整产品姿势、交互 Maya 回调与持续播放未由单关节 witness 证明。 |
| User Story 21：无贴图关卡参考 | [#53 当前汇总](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/53#issuecomment-5888512464)、[记录](issue-53-scene-reference-acceptance.md) | 显式范围、FBX 与产物图片检查、世界测量、灰模和失败恢复通过。最终独立 review-03 在源归档 `final_review`；对象类别／深层外来节点／真实崩溃边界保留。 |
| User Stories 22–23：独立配对与目标骨架 | [#54 当前汇总](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-5883765955)、[记录](issue-54-multi-subject-acceptance.md) | C01＋C02 真实双对象及合成 full-body／arms、严格必要骨骼映射、世界 oracle、独立移除和最小 Sequence 恢复通过。review-04 原评论 6136464547 在源归档；产品接入与多对象缓存未实施。 |
| User Story 24 与共同规划决定 | 全部七项记录及[整合草案](../../../composite/MtoULiveLink/docs/preview-workflow-integration-plan.md) | 双端、协议、恢复和实际宿主证据按原验收范围复用；三个原型联合设计现在具备输入。新的组合路径仍须在后续实施中独立验收。 |

### 审核问题

无阻塞 #47 原约定范围的未解决问题。未发现需重开子任务的基线漂移。
路线图中已过时的 #51 自然 tick／视觉待验收、#52 日常 UE 编辑器跟随待验证等
描述已按最终记录更正，v7／v8 的阶段表述统一为当前产品 v9。

以下归属于后续整合；`#52/R-001` 等引用保留原子项 ID，未冒充新的父项缺陷：

| 来源 | 类型 | 当前处理及完成条件 |
| --- | --- | --- |
| #52/R-001、R-002 | 验收缺口（后续产品范围） | 草案 INT-09／10 要求真实交互 Maya 回调生命周期与持续播放测量；当前 #52 已接受范围不变。 |
| #52/R-004、R-005、R-006；#53／#54 产品候选 | 后续候选 | 草案统一空间、时间、会话／对象／求值身份和控制恢复，明确相机临时策略与退出帧；多对象缓存／MRQ 另定范围。 |
| 三原型分别成立、未联合运行 | 待验证风险 | 草案 INT-01–12 要求同一场景下的相机、参考和完整角色／道具联合验收，不能仅拼接现有测试计数。 |
| #53 容器深层外来节点、#54 任意生产 Sequence／writer | 待验证风险 | 产品化前明确拒绝与安全更新／恢复规则，使用真实已有 Sequence 和外来节点故障输入取证，不扩张原型通过范围。 |

### 验证与证据边界

**本轮执行（Windows PowerShell、CPython 3.13.14；未启动 Maya／UE）：**

| 检查 | 结果 |
| --- | --- |
| `python -m unittest discover -s composite/MtoULiveLink/maya/MtoULiveLink/tests -p test_mtou_livelink.py -v` | 158/158，无跳过 |
| `python -m unittest discover -s <prototype>/tests -t <prototype> -v`，camera-sync 的 Maya 组件 | 63/63，无跳过 |
| 同上，scene-reference 的 Maya 组件 | 110/110，无跳过 |
| 同上，multi-subject 的 Maya 组件 | 23/23，无跳过 |
| `python composite/MtoULiveLink/protocol/generate_unreal_corpus.py --check` | 通过，v9 corpus 与 UE 生成源一致 |
| `python tools/validate_repository.py` | 通过，无错误／警告 |
| `python -m unittest discover -s tests -v` | 37/37，无跳过 |
| `python tools/issue_report.py check-archive <archive> --json`，#52／53／54 源评论归档 | 三份通过 |

上述 `<prototype>` 分别为
`composite/MtoULiveLink/prototypes/camera-sync/maya/MtoUCameraSyncPrototype`、
`composite/MtoULiveLink/prototypes/scene-reference/maya/MtoUSceneRefPrototype`、
`composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype`。
354 项产品／原型纯测试与 37 项仓库测试分开统计，不把它们称为 391 项宿主验收。
文档最终 diff、相对链接与报告结构在发布前核对；命令输出不作为新产品功能证据。

**基线复核：** 对每行执行 `git merge-base --is-ancestor <base> HEAD` 和
`git diff --name-only <base> HEAD -- <owned paths>`；以下基线都是当前历史的祖先。

| 范围 | 已验收实现基线（均已远端发布） | 相比 checkout 的差异 |
| --- | --- | --- |
| 主产品 Maya／UE／protocol | `12199b15f8134e533a44e21a62744d22b0cff2b5` | 三个目录完全一致，包含 #48–51 最终结果。 |
| #52 Maya／UE | `25336faee2e03076e7e22e829a9793509b1cfbca` | 两个组件目录完全一致。 |
| #53 Maya／UE | `e2462d92e63f25f13212f4e2aa9d4e55c17f032b` | 两个组件目录完全一致。 |
| #54 Maya／UE | `b593dad42849705873c4d1b59f633992a966f4e3` | 仅 UE README 在收尾时调整，全部运行源码与测试一致。 |

#48／49 原验收 `29beaa88bd2907fb1c5b7e79dd3b490095a94c22` 后的主产品变化由
#50／51 验收覆盖。#51 的 `867f2994b5b22febe2afd9e712e303453c6cdeb7` 至当前，
主产品仅新增 #50 已单独编译／复验的 Details 整数输入修复 `12199b1`；
没有把未审的新运行代码混入历史宿主结果。

**历史复核：** Maya 2024、stock UE 5.7.4 等宿主结果来自各子项独立审核。
本轮检查 #52 JSON 的 UE 11 成功／0 失败及 Maya 145 checks／0 failures，
#53 最终证据的实现基线及 110／299 记录，#54 review-04 基线及分项结果；
归档保留原报告身份与时间戳。没有重新构建引擎模块或重跑双宿主、截图、打包。
运行源码相同且本轮只改文档，是复用对应范围历史宿主证据的依据。

保留这些限制：#50 原完整 suite 原始文件曾未在旧路径找到，依据是已发布独立
审查及维护记录，不能宣称本轮取回该原始文件；#52 秒级 fixture 不证明持续性能；
#53 若缺 mayapy 参数，RealMayaPeer 可跳过，不能只看 Success；#54 总套件
17 Success 中实际执行 16，RealAssetPair 后来单独实际运行。真实资产的加载警告
及预期负向错误不是新缺陷；#54 混合位移／弧度／scale 残差不标为纯厘米。

### 后续处理

发布本次验收与当前汇总后可关闭 #47。准备的整合草案按 I-1（单角色＋相机＋
参考完整闭环）、I-2（双对象及接管）、I-3（日常恢复与交付）承接后续实施，
每个切片包括双端、协议和验收。新草案中的建议性能预算与范围需在实施规格冻结。
本会话不启动整合、不修改运行代码、现行 ADR、版本、主流程入口或协议。

本次不是 #48–54 的归档收尾：既有评论、源归档和其他任务 scratch 保留。
仅本轮报告草稿、快照和测试输出在发布验证后删除，持久结论留在本记录和方案中。
