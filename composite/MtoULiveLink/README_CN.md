# MtoU_LiveLink

[English](README.md)

## 项目简介

MtoU_LiveLink 是一个 Maya 与 Unreal 复合插件，可通过本机 Live Link 连接，
在 Unreal Editor 中预览 Maya 已求值的骨骼动画和 BlendShape。插件支持实时动画
预览、缓存播放和服装模型预览，不会创建动画或预览资产。

## 支持版本

- 64 位 Windows
- Autodesk Maya 2022.4
- 官方原版 Unreal Editor 5.7.4
- Topia Engine 5.7.4（已验证 Win64 插件编译和编辑器加载）
- 仅限 Unreal 编辑器。不支持 PIE（Play In Editor）：PIE 世界不会参与流送目标发现。

不保证兼容其他第三方修改版 Unreal Engine 5.7。

## 安装方式

1. 将 `maya/MtoULiveLink/scripts/MtoULiveLink.py` 复制到 Maya 脚本目录，
   或直接在 Maya Python 脚本编辑器中执行。
2. 将 `unreal/MtoULiveLink/` 复制到 `<Project>/Plugins/MtoULiveLink/`。
3. 官方原版 Unreal 按正常方式编译项目。使用 Topia Engine 5.7.4 时，先关闭
   Unreal Editor，将 `TOPIA_ENGINE_ROOT` 设为包含 `Engine` 的目录，将
   `ATHENA_UPROJECT` 设为目标 `.uproject`，再从仓库根目录运行：

   ```powershell
   pwsh ./tools/build_mtou_topia.ps1 `
     -EngineRoot $env:TOPIA_ENGINE_ROOT `
     -ProjectFile $env:ATHENA_UPROJECT `
     -Apply
   ```

   不带 `-Apply` 时只校验路径并预览将生成的五个文件；需要机器可读结果时添加
   `-Json`。Topia 构建会把所有可写中间状态放入临时目录，只向已复制的
   MtoULiveLink 插件写入 `Binaries/Win64`，不会修改引擎文件或项目源码、配置。
4. 启用 **Live Link** 和 **MtoU_LiveLink**，然后重启 Unreal Editor。

Maya 与 Unreal 组件必须来自同一版本。

## 使用方法

1. 在 Unreal 中创建 **MtoU_LiveLink Binding**，指定 **Driver Skeletal Mesh**，
   然后将该 Binding 资产从内容浏览器拖入关卡。此操作会自动创建
   MtoU_LiveLink Binding Actor 并指定该 Binding；Actor 的 Binding 引用不是
   可编辑的设置字段。
   该 Actor 所属的插件显示组件在显示 Maya 已求值数据时会绕过 Driver Skeletal
   Mesh 的 Post Process Anim Blueprint；Driver 资产和其他生产组件仍保持各自行为。
   可通过细节面板顶部的 **MtoU** 分类仅查看插件控件。刷新后，**Modified parts**
   会逐行列出替换 Driver 几何体的 Preview 材质槽；三角形数量、耗时和质量阈值等
   内部指标不会显示在面向美术人员的面板中。点击 **Delete Preview** 会释放生成的
   预览网格体并立即恢复显示 Driver Skeletal Mesh。
   渲染设置只需在 **SkeletalMeshComponent** 上配置一次。内部 Driver 显示组件不会
   再出现在细节面板中；模型预览需要同时显示两个网格体时，它会继承光照通道和
   动态内嵌阴影设置。
2. 在 Maya 中运行 `MtoULiveLink.py`，选择一个变形根骨骼，点击“设置角色”，
   并确认检测到的 Display 控制器、服装、场景帧率和传输上限。
3. 选择“动画”或“模型”，然后点击“连接”。
4. 在“动画”模式中，可通过 Maya 摆姿、拖动时间轴或播放动画进行实时预览；
   如需复查一段动画，选择“缓存播放”并点击“捕获并回放”。捕获或上传失败时会
   自动恢复实时预览并显示原因；回放运行失败会保留缓存，可重试或退出缓存模式。
   捕获一超过固定的 20,000 帧或 1 GiB 上限就会立即停止。
5. 在“模型”模式中，还需在 Unreal 指定服装 **Preview Static Mesh**，连接前点击
   **Refresh Preview**；开启“传递 BS”可传输名称匹配的 BlendShape。关闭
   “传递 BS”时，连接会标注为不可用于模型验收的 bone-only 诊断；开启
   “传递 BS”但服装未声明 BlendShape 时，属于有意的仅骨骼驱动，会以空接受集
   正常进入 Ready。两种情况下服装预览都仅由骨骼驱动，不驱动任何 Morph Target。
   使用整角色 Driver 时，模型预览会保留显示身体、脸、头发及其他非服装
   材质槽，并用生成的 Preview 替换解析出的原服装材质槽；Driver 独有的脸部、
   头发 Morph Target 也会继续接收 Maya 中的同名曲线。服装与非服装几何体应使用
   不同的导入材质槽；若二者共用一个槽，Refresh 会报告问题，而不会隐藏角色的
   其他部分。Refresh 失败时会保持 Error 状态且无可用预览，但会恢复显示已绑定的
   Driver 以便检查；在下一次 Refresh 成功之前，模型连接仍会被阻止。
   Auto 使用保守的拓扑密度限制：Driver 有多个连通区域且 Preview 至少有 8 个
   三角形时，选中的 Driver 三角形数不能超过 Preview 的 1.70 倍。
   即使对齐且表面完全相同，减面也可能超限；该诊断不能证明存在重复或身体几何。
   请检查源模型，删除实际存在的重复副本，并拆分服装与身体混用的材质槽。
   对于有意减面的 Preview，在 Binding 的 **Driver Garment Slot Override** 中
   仅指定服装独立的材质槽，再点击 **Refresh Preview**。手动选择仍会检查几何、
   整个 Preview 的覆盖率、对齐、共用槽和传递质量。可用的黄色 Warning 仍需检查
   后才能验收。
6. 更换服装、工作流或“传递 BS”设置后，请重新连接。更改 Driver Skeletal
   Mesh 或 Preview Static Mesh（包括重新导入）、删除 Binding Actor、或卸载其
   所在的 Editor 关卡时，当前 Streaming 会话会立即结束且当前 Preview 版本失效：
   请重新点击 **Refresh Preview** 并重新连接。
