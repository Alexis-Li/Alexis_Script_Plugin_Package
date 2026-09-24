# MtoU_LiveLink

[English](README.md)

MtoU_LiveLink 连接同一台电脑上的 Maya 与 Unreal Editor，让你在 Maya 中制作时，
直接在 Unreal 中查看角色动画、BlendShape 和服装修改效果。支持实时预览与动画
缓存播放，不会创建动画或预览资产。

## 兼容性

- 64 位 Windows
- Autodesk Maya 2024（当前工作流程）；继续支持 Maya 2022.4
- 官方原版 Unreal Editor 5.7.4
- Topia Engine 5.7.4（已验证插件编译与编辑器加载）
- 仅用于编辑器，不支持 PIE（Play In Editor）。

不保证兼容其他第三方修改版 Unreal Engine 5.7。Maya 与 Unreal 组件应使用同一版本。

## 安装

以下组件路径均相对于本产品目录。

1. 将 `maya/MtoULiveLink/scripts/MtoULiveLink.py` 复制到 Maya 脚本目录，
   或直接在 Maya 的 Python 脚本编辑器中执行该文件。
2. 将 `unreal/MtoULiveLink/` 复制到 `<Project>/Plugins/MtoULiveLink/`。
3. 编译 Unreal 项目。Topia Engine 用户请按下方说明操作。
4. 启用 **Live Link** 和 **MtoU_LiveLink**，然后重启 Unreal Editor。

### Topia Engine 5.7.4

**推荐：双击向导（Windows）**

关闭 Unreal Editor，确认项目 `Plugins/MtoULiveLink` 已放入源码插件。
双击仓库中的 `tools/build_mtou_topia_gui.cmd`，选择项目 `.uproject`，再选择公司引擎的
`Engine/Binaries/Win64/UnrealEditor.exe`。核对显示的路径后点击“确定”，等待成功提示再打开项目。
失败时将控制台中的错误及 `Build log` 路径交给技术同事。
若提示缺少编译器或 SDK，请由技术同事配置，
或提供与同一公司引擎版本匹配的已编译插件。
编译完成后会替换目标插件的编译产物。
如需单独复制向导，请将 `build_mtou_topia_gui.cmd`、`build_mtou_topia_gui.ps1` 和
`build_mtou_topia.ps1` 放在同一文件夹中。

**命令行方式**：关闭 Unreal Editor。将 `TOPIA_ENGINE_ROOT` 设为包含 `Engine` 的目录，
将 `ATHENA_UPROJECT` 设为目标 `.uproject`，然后在仓库根目录运行以下命令：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./tools/build_mtou_topia.ps1 `
  -EngineRoot $env:TOPIA_ENGINE_ROOT `
  -ProjectFile $env:ATHENA_UPROJECT `
  -Apply
```

去掉 `-Apply` 可先检查路径并预览安装内容。

## 准备角色

- Maya 必须包含启用网格实际蒙皮所需的骨骼及完整祖先链，包括骨骼之间的中间
  分组，名称与父子关系应一致。同一短名出现在多个 Maya 分支时，支持同一父级下
  的 Unreal 导入数字后缀或 32 位 hash 后缀，并报告映射。无关的导出骨骼分支
  不会阻止连接；但每根 Unreal 骨骼只由一根 Maya 骨骼驱动，若同一父级下有两根
  Maya 骨骼发布同一名称，请在 Maya 中重命名其中一根。
- 预览服装时，准备与 Primary Driver 服装位置对齐的 **Preview Static Mesh**。
  服装与身体等非服装几何体应使用不同的导入材质槽。
- 预览 BlendShape 时，Maya BlendShape 与 Unreal Morph Target 需要名称匹配，
  并在 Maya 中开启“传递 BS”。

### 拆分角色部件

当同一个角色由多个 Skeletal Mesh 组成时，将服装数据来源指定为 Primary Driver，
其余网格添加到 Binding 的 **Additional Parts**。所有启用部件在同一个连接下摆姿和显示：

- 只有 Primary Driver 提供服装识别、权重转移和 **Preview Morph transfer**。
- 每个部件包含名称、Skeletal Mesh 和 **Enabled** 开关。所有网格必须共用同一个
  **Skeleton** 资产。各 LOD 实际蒙皮所需的共有骨骼及其完整祖先链，必须具有一致
  的层级和参考姿势。部件独有的次级骨骼即使不在 Primary 中，也会接收 Maya 动画；
  禁用部件不增加骨骼依赖。
- 新增服装及次级骨骼后，导入新服装、更新启用部件并重连。旧网格仅在自身绑定
  姿势或权重改变时需要更新。Maya 缺少必要骨骼或共有骨骼不兼容时仍会拒绝连接，
  并提示具体部件和骨骼。
- 只有某个部件拥有的 Morph Target 也会传输；多个部件同名的 Morph 会接收到
  相同数值。
- 新增、删除、启用、停用或替换部件都会结束当前连接，之后需在 Maya 中重连。
  已生成的服装预览会保留；Primary Driver、Preview Static Mesh、
  **Driver Garment Slot Override** 或相关导入数据变更后需要 **Refresh Preview**。
- 重命名部件或调整列表顺序无需重连。

## 首次连接

1. 在 Unreal 中创建 **MtoU_LiveLink Binding** 资产，指定
   **Primary Driver Skeletal Mesh**，并按需添加 **Additional Parts**。
2. 将 Binding 资产从内容浏览器拖入关卡，创建对应的 Binding Actor。
   细节面板的 **MtoU Preview** 用一个 **Status** 区显示预览就绪状态、连接、
   下一步和当前显示网格，下方是 **Refresh Preview** 与 **Delete Preview**。
   需要部件、预览、模型或原始连接详情时再展开 **MtoU Diagnostics**。
3. 在 Maya 中运行 `MtoULiveLink.py`，选择变形根骨骼，点击“设置角色”。
   顶部切换“动画”／“模型”，下方依次是连接控制（设置角色、连接、断开与连接
   状态）、场景信息（左列根骨骼、衣服、帧率；右列骨骼／BlendShape 数量与缓存）、
   预览与播放（预览模式、传输上限，缓存按钮仅在缓存播放模式下出现）、工具
   （Display 和重名骨骼选择）、诊断（状态与诊断详情）。警告开关位于诊断卡片下方。
4. 选择“动画”，点击“连接”，指示变绿。在 Maya 中摆姿、拖动时间轴或播放
   动画，即可在 Unreal 中查看效果。预览行的“上限”用于限制实时传输帧率。
   不可用按钮会用提示说明原因。同一连接的相同警告只打断一次，新错误仍会
   弹出。

## 动画预览

使用“动画”模式实时预览角色。需要查看一段缓存动画时，先设置 Maya 时间轴的
播放范围，再选择“缓存播放”并点击“捕获并回放”。捕获和上传完成后开始播放。
缓存按钮仅在“缓存播放”模式下出现在预览与播放分区中。诊断状态行可区分
捕获中、上传中、回放中、完成停在最后一帧、已停止保留缓存和失败。

单次捕获上限为 20,000 帧或 1 GiB，达到上限时请缩短范围。捕获或上传失败会
返回实时预览并显示原因；回放失败时可以重试保留的缓存，或退出缓存模式。

## 服装模型预览

1. 在 Unreal Binding 中指定服装 **Preview Static Mesh**。
2. 在 Binding Actor 上点击 **Refresh Preview**，等待 **Status** 显示 Ready、
   Ready with a warning 或 Refresh failed。**MtoU Diagnostics** 中的
   **Modified parts** 会列出预览替换的材质槽，Driver 角色的其他部分仍会显示。
3. 在 Maya 中选择“模型”，确认服装和“传递 BS”设置，再点击“连接”。
4. 在 Maya 中给角色摆姿，检查 Unreal 中的服装变形。

## 常见问题

| 问题 | 处理方法 |
| --- | --- |
| 骨架不匹配 | 先按详情中的根因排查：首个未映射骨骼的完整路径与父级、受阻的 Maya 后代与未访问的 Unreal 骨骼数量，以及提示的导入改名候选；若详情指出同一必要骨骼由两根 Maya 骨骼竞争，请在 Maya 中重命名其中一根。再检查 Maya 选择的根骨骼、层级，以及 Unreal Driver 网格是否为最新版本。 |
| 附加部件被拒绝 | 诊断信息会指出具体部件和原因。请检查是否共用同一个 **Skeleton** 资产、Maya 中提示的必要骨骼及父链是否完整，以及共有参考姿势是否一致。若无法读取蒙皮权重，保留 CPU 蒙皮数据后重新构建或导入网格。 |
| 预览刷新失败 | 检查网格对齐情况、服装与身体的材质槽是否分离，修正提示的问题后重新刷新。 |
| 有意减面的模型无法通过自动服装识别 | 检查源模型是否有重复几何体或服装／身体混用材质槽。确认是有意减面后，在 Binding 的 **Driver Garment Slot Override** 中指定服装独立的材质槽，再刷新并检查效果。 |
| BlendShape 未生效 | 检查“传递 BS”、名称匹配、当前服装及连接诊断。模型预览的源资产变更后需重新刷新。 |
| 修改后连接断开 | 更换服装、模式、“传递 BS”或角色部件后需重连；刷新也会断开当前连接。修改或重新导入 Primary Driver、Preview Static Mesh，或修改 **Driver Garment Slot Override** 后，刷新模型预览并重连；撤销／重做这些修改时也遵循同一规则。 |

预览时请保留 Binding Actor 并保持其关卡加载；删除 Actor 或卸载关卡会结束连接。

## 更多信息

- [版本记录](CHANGELOG.md)
- [开发与验收记录](../../docs/project-history/mtou-livelink/README.md)
