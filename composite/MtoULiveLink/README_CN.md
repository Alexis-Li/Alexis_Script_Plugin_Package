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

1. 在 Unreal 中创建 **MtoU_LiveLink Binding**，指定 **Driver Skeletal Mesh**，
   并可在其下方指定服装 **Preview Static Mesh**。确保关卡中只有一个 Binding
   Actor，并把 Binding 指定给该 Actor。需要模型预览时，选中 Actor 并显式点击
   **Refresh Preview**。Refresh 只读取 LOD0 源数据并生成 Actor 自有的瞬态数据，
   对 Preview 中存在对应表面的 Driver Morph 进行投射；若某个局部 Morph 对应的表面
   已不在 Preview 中，则跳过该 Morph 并显示警告。输入修改或源资产重建会将其标记为
   Dirty，必须再次显式 Refresh。对于损坏或构建失败的 Morph，Refresh 仍保持事务性，
   不会留下局部生成的 Preview。
2. 在 Maya 中运行 `MtoULiveLink.py`，选择唯一的变形根骨骼，再点击
   “设置角色”。工具会自动寻找 `Display_ctrl` 和 Clothes 枚举；
   若候选不唯一，请使用手动 Display 按钮。
   若骨架中存在发送后会冲突的重名骨骼，可在错误弹窗或主窗口点击
   “选中重名骨骼”，一次选中所有冲突 joint，再在大纲视图中定位处理。
   重名不会立即阻止连接；UE 会按已匹配父级寻找导入时自动追加数字的
   唯一骨骼名。映射成功时连接并警告，无法唯一映射时才拒绝连接。
3. 确认当前衣服和场景帧率，选择播放传输上限（**Follow Scene**、
   **30 fps**、**20 fps** 或 **15 fps**），再点击“连接”。默认值为
   **20 fps**，选择会保存到 Maya 原生 optionVar 中。
4. 使用顶部的 **动画** 和 **模型** 按钮选择工作流；启动时始终默认
   **动画**。切换工作流会断开当前会话并清除动画缓存播放，同时保留已设置的
   根骨骼、Display 控制器和当前衣服；整个过程不会修改 Maya 场景。
   **模型** 工作流显示同样的角色、Display/服装、帧率、传输上限、连接状态和
   诊断控件，并额外提供默认开启的 **传递 BS** 开关，同时隐藏缓存播放控件。
   在 **模型** 工作流连接时，要求 UE 的 Binding Actor 已生成可用的 Generated
   Preview Skeletal Mesh；否则连接会被 `PREVIEW_NOT_READY` 拒绝，且可见目标
   不会发生任何变化。关闭 **传递 BS** 时，可连接该预览，但会明确标记为
   “仅骨骼诊断，不可用于模型验收”；开启 **传递 BS** 时，UE 只接受当前 Maya
   服装 BlendShape 与 Generated Preview Morph 库的交集。交集为空时会以
   `PREVIEW_MORPH_MISMATCH` 拒绝连接；部分覆盖会连接并显示黄色警告及双方差异
   列表，完全覆盖则进入 Ready。只传输已接受的数值，因此仅 UE 存在的 Morph
   始终保持为零。
5. 在 Maya 中调整姿势、播放或拖动时间轴。上限只在 Maya 播放期间生效；
   暂停时的摆姿和手动拖动仍按场景帧率采样，停止播放会立即提交最终姿势。
   连接期间切换 Clothes 枚举或 **传递 BS** 都会断开会话；换装后请在 UE 中
   替换对应 Binding Actor，再重新连接以完成新的协商。
6. 在 **动画** 工作流中，使用互斥的 **实时预览** 和 **缓存播放** 模式进行
   复查。缓存播放要求连接已完成 Unreal 协商，并在缓存使用期间暂停实时采样。
   点击 **捕获并回放** 后，工具会按当前 Maya Playback Range（包含首尾帧）
   逐帧采样，把 protocol-v6 缓存帧增量写入当前用户的系统临时目录并恢复原来的
   当前帧，然后在无实时时限的情况下把完整缓存上传给 Unreal。捕获会显示
   当前/总进度，开始前自动停止 Maya 播放，并可点击取消。工具会在写入帧前估算
   临时磁盘用量，超过 1 GiB 时请求确认，空间不足时拒绝捕获；失败或取消都会删除
   未完成的缓存。Unreal 会校验上传的元数据、帧数、序列、变换、曲线和资源上限，
   只有完整接收并缓冲后才会回复 **cache ready**；过大或非法的上传会收到稳定的
   错误并被整体拒绝，绝不会进入回放。
7. Unreal 回复就绪后本地回放自动开始：由 Unreal 使用自身单调时钟按捕获时记录的
   场景帧率驱动，把每个缓存帧恰好一次、按顺序地应用到角色上；期间 Maya 不发送
   任何逐帧动画数据。传输进度与回放进度分开显示。成功完成会停在最后一帧；
   若跟不上捕获帧率，回放会停止并明确报告播放性能失败——Unreal 既不会静默跳帧，
   也不会静默拉长已完成的复查。点击 **停止回放** 停在最后已应用的帧并保留缓存；
   点击 **再次回放** 直接复用已上传的兼容缓存而不重新捕获。切回 **实时预览**
   会停止本地回放、清空 Unreal 临时缓冲，并立即提交 Maya 当前姿势。缓存播放不会
   创建 Unreal 资产；已完成缓存只保留在兼容的 Maya 会话中，并会在替换、取消、
   断开连接、场景或角色变化、工具关闭或 Maya 退出时删除。下次启动只会清理有效且
   超过 24 小时的 MtoU 缓存残留。

“查看诊断详情”只显示当前错误的摘要、解决办法、错误代码和相关技术详情。
长内容会按窗口宽度自动换行，主窗口已有的角色与场景信息不会重复显示。

接收端会保留每个已放置 Actor 的变换，且不会创建 Animation Sequence。Binding
Actor 会在 Unreal 编辑器中持续更新动画；连接期间，插件会临时强制关卡视口进入
“实时”模式，并在断开连接后恢复各视口原先的设置。断开连接会清除最后一个传输帧，
使模型回到参考姿势，而不是继续保留旧姿势。Maya 保存的 SkinCluster Bind Pose 与当前动画帧
会分别处理，并映射到目标 Skeletal Mesh 的 Reference Pose；连接时无需让第 1 帧或当前帧为
A Pose。未保存绑定数据的关节（例如绑定后添加的矫正滑杆关节）会使用设置角色时的姿势。
各 SkinCluster 绑定矩阵不一致时（换装在不同姿势下绑定即会出现），会按关节所连的
Bind Pose `dagPose`（即 Go to Bind Pose 恢复的姿势）解析，无 dagPose 时取 influence
数最多的 SkinCluster，并在每次角色捕获后显示已解析的冲突数量。若最高优先级候选
并列且矩阵不一致，会停止角色设置，不再由 SkinCluster 节点名称决定姿势；非有限或
不可逆的绑定矩阵会在求逆前被拒绝。多个蒙皮网格部件上的同名 BlendShape 在求值
一致时会合并为一条 Unreal 曲线发送；若数值不同，采样会停止并列出所有冲突的 Maya
插口。Maya 和 Unreal 单方存在的 BlendShape 会作为不阻断连接的警告显示。
协议版本 6 要求 Maya 和 Unreal 两端组件配套安装：`init` 携带所选的
**动画**/**模型** 工作流和 **传递 BS** 选择，`ready` 回显工作流并上报目标与
已接受的 Morph 数量；缓存播放消息（建立缓存归属的 `cache_enter`、携带上传身份与权威快照修订版的
`cache_begin`、带索引的 `cache_frame`、`cache_end`、回显身份的 `cache_ready`、
携带播放身份的 `cache_play`、有界 `cache_progress`、包含已应用帧数与耗时的
`cache_complete`、`cache_stopped`、`cache_cleared`）负责传输并控制 Unreal 侧的
瞬态缓存，并提供稳定的上传、修订、资源上限与播放性能错误。Unreal 会按声明尺寸
与冻结上限计量实际上传字节，依据协商数量预检解析内存，每个更新至多应用一个源帧
位置的姿势，并在错过窗口时于任何追赶式连发之前停止。protocol-v5 客户端会收到
正常的版本不匹配错误而拒绝连接。
模型工作流额外使用稳定的 `PREVIEW_NOT_READY`、`PREVIEW_BUILD_FAILED` 和
`PREVIEW_MORPH_MISMATCH` 错误。UE 显式 Refresh 后，启用 BlendShape 的模型预览
会传输 Maya 与 Generated Preview 的非空交集；部分覆盖明确显示为黄色，完全覆盖
则进入 Ready。仅骨骼路径仍有明确标记，且不可用于模型验收。
