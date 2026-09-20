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

- Maya 的变形骨架应与 Unreal 的 **Primary Driver Skeletal Mesh** 匹配，包括骨骼
  名称和父子关系；骨骼之间的中间分组也会参与匹配。同一短名出现在多个 Maya 分支
  时，只要 Unreal 导入在相同父级下为该重名骨骼添加了数字后缀或 32 位 hash 后缀
  即可连接，连接会报告每一处改名映射。插件不提供自动重定向，Maya 增删骨骼后需
  同步更新 Unreal 资产。
- 预览服装时，准备与 Primary Driver 服装位置对齐的 **Preview Static Mesh**。
  服装与身体等非服装几何体应使用不同的导入材质槽。
- 预览 BlendShape 时，Maya BlendShape 与 Unreal Morph Target 需要名称匹配，
  并在 Maya 中开启“传递 BS”。

### 拆分角色部件

当同一个角色由多个 Skeletal Mesh 组成时，将保留完整变形层级的网格指定为
Primary Driver，其余网格添加到 Binding 的 **Additional Parts**。所有启用部件
会在同一个连接下作为同一个角色摆姿和显示：

- 只有 Primary Driver 定义骨架基准，并提供服装识别、权重转移和
  **Preview Morph transfer**；身体、面部和 BlendShape 预览仍通过它生效。
- 每个部件包含名称、Skeletal Mesh 和 **Enabled** 开关。部件必须与 Primary
  Driver 共用同一个 **Skeleton** 资产，其骨骼需按名称与父路径映射到 Primary，
  各 LOD 中影响部件蒙皮的骨骼及其父链需匹配 Primary 的参考姿势。
  部件可以使用更少的骨骼和不同的几何体；
  Primary 中不存在的骨骼会被拒绝，并提示部件与骨骼名称。
- 只有某个部件拥有的 Morph Target 也会传输；多个部件同名的 Morph 会接收到
  相同数值。
- 新增、删除、启用、停用或替换部件都会结束当前连接，之后需在 Maya 中重连。
  已生成的服装预览会保留，只有在 Primary Driver、Preview Static Mesh 或其导入
  数据变更后才需要 **Refresh Preview**。
- 重命名部件或调整列表顺序无需重连。

## 首次连接

1. 在 Unreal 中创建 **MtoU_LiveLink Binding** 资产，指定
   **Primary Driver Skeletal Mesh**，并按需添加 **Additional Parts**。
2. 将 Binding 资产从内容浏览器拖入关卡，创建对应的 Binding Actor。
   在细节面板的 **MtoU** 分类中操作插件控件；其中的 **Character parts** 行会
   列出解析后的角色组成，并提示无法加入角色的部件。
3. 在 Maya 中运行 `MtoULiveLink.py`，选择变形根骨骼，点击“设置角色”。
   检查检测到的 Display 控制器、服装、场景帧率和传输上限。
4. 选择“动画”，点击“连接”。在 Maya 中摆姿、拖动时间轴或播放动画，
   即可在 Unreal 中查看效果。

## 动画预览

使用“动画”模式实时预览角色。需要查看一段缓存动画时，先设置 Maya 时间轴的
播放范围，再选择“缓存播放”并点击“捕获并回放”。捕获和上传完成后开始播放。

单次捕获上限为 20,000 帧或 1 GiB，达到上限时请缩短范围。捕获或上传失败会
返回实时预览并显示原因；回放失败时可以重试保留的缓存，或退出缓存模式。

## 服装模型预览

1. 在 Unreal Binding 中指定服装 **Preview Static Mesh**。
2. 在 Binding Actor 上点击 **Refresh Preview**，检查刷新结果。
   **Modified parts** 会列出预览替换的材质槽，Driver 角色的其他部分仍会显示。
3. 在 Maya 中选择“模型”，确认服装和“传递 BS”设置，再点击“连接”。
4. 在 Maya 中给角色摆姿，检查 Unreal 中的服装变形。

评估模型效果时保持“传递 BS”开启。关闭后仅用于骨骼驱动诊断，不能验证
BlendShape 变形。没有 BlendShape 的服装可正常使用骨骼驱动预览。

出现黄色 **Warning** 时，请检查效果后再确认结果；出现 **Error** 时，需要
**Refresh Preview** 成功后才能连接模型模式。较大的网格刷新可能需要数秒，
请等待完成。

点击 **Delete Preview** 可移除生成的预览，恢复 Driver 网格显示。
光照和阴影设置统一在 **SkeletalMeshComponent** 上配置。

## 常见问题

| 问题 | 处理方法 |
| --- | --- |
| 骨架不匹配 | 先按详情中的根因排查：首个未映射骨骼的完整路径与父级、受阻的 Maya 后代与未访问的 Unreal 骨骼数量，以及提示的导入改名候选；再检查 Maya 选择的根骨骼、层级，以及 Unreal Driver 网格是否为最新版本。 |
| 附加部件被拒绝 | 诊断信息会指出具体部件和原因。请检查该部件是否与 Primary Driver 共用同一个 **Skeleton** 资产、提示的骨骼是否在 Primary 中以相同的父骨骼存在，以及参考姿势是否匹配。 |
| 预览刷新失败 | 检查网格对齐情况、服装与身体的材质槽是否分离，修正提示的问题后重新刷新。 |
| 有意减面的模型无法通过自动服装识别 | 检查源模型是否有重复几何体或服装／身体混用材质槽。确认是有意减面后，在 Binding 的 **Driver Garment Slot Override** 中指定服装独立的材质槽，再刷新并检查效果。 |
| BlendShape 未生效 | 检查“传递 BS”、名称匹配、当前服装及连接诊断。模型预览的源资产变更后需重新刷新。 |
| 修改后连接断开 | 更换服装、模式、“传递 BS”或角色部件后需重连；刷新也会断开当前连接。修改或重新导入 Primary Driver 或 Preview Static Mesh 后，刷新模型预览并重连。 |

预览时请保留 Binding Actor 并保持其关卡加载；删除 Actor 或卸载关卡会结束连接。

## 更多信息

- [版本记录](CHANGELOG.md)
- [开发与验收记录](../../docs/project-history/mtou-livelink/README.md)
