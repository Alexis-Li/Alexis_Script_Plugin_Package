# Alexis 脚本与插件合集

[English](README.md)

## 这个仓库是什么

这是一个面向 3D 美术生产的 Autodesk Maya 脚本与工具合集，每个项目都可以
独立使用；仓库同时提供独立 Unreal Engine 插件，以及跨多个宿主软件协作的复合
产品开发工作区。

从下表选择工具，按对应手册确认支持的 Maya／Python 版本、安装方式和使用步骤。
各工具及宿主组件均可独立安装。

## 包含的工具

| 工具 | 用途 | 位置 |
| --- | --- | --- |
| Flatten Mesh To UV | 将模型的 UV 展开结果转换为平面多边形。 | [`maya/tools/FlattenMeshToUV`](maya/tools/FlattenMeshToUV/) |
| MtoU Live Link | 通过可独立安装的 Maya 和 Unreal 组件，将一名已求值 Maya 角色流式发送到官方版 Unreal Live Link。 | [`composite/MtoULiveLink`](composite/MtoULiveLink/) |
| NitroPoly | 提供选择、拓扑、轴心、连接和循环边等多边形建模功能。 | [`maya/tools/NitroPoly`](maya/tools/NitroPoly/) |
| ZiSpread | 通过交互拖动均匀分布所选环线。 | [`maya/scripts/ZiSpread`](maya/scripts/ZiSpread/) |

## 工具文档

- [Flatten Mesh To UV](maya/tools/FlattenMeshToUV/README_CN.md)
- [MtoU Live Link](composite/MtoULiveLink/README_CN.md)
- [NitroPoly](maya/tools/NitroPoly/README_CN.md)
- [ZiSpread](maya/scripts/ZiSpread/README_CN.md)
- [Maya 项目指南](docs/maya-development.md)
- [命名规范](docs/naming-conventions.md)
- [发布流程](docs/release-process.md)
- [项目开发历史](docs/project-history/README.md)

## 开始开发

1. 安装 Python 3.10 或更高版本，用于运行仓库维护命令。
2. 修改前先阅读 [`AGENTS.md`](AGENTS.md) 和对应平台目录内的 `AGENTS.md`。
3. 先预览项目模板，确认无误后再加 `--apply` 创建文件：

   ```powershell
   python tools/create_project.py maya-script SampleTool --json
   ```

4. Maya 用户直接运行的项目目录和入口文件使用 PascalCase，例如
   `FlattenMeshToUV` 和 `FlattenMeshToUV.py`。
5. 修改仓库工具或目录结构时，运行仓库检查：

   ```powershell
   python -m unittest discover -s tests
   python tools/validate_repository.py
   ```

   修改单个项目时，只运行该项目自身的测试、静态检查、构建和打包检查。

通用流程见[开发约定](docs/development-conventions.md)，Unreal 项目导航见
[Unreal 插件指南](docs/unreal-development.md)。按
[Issue／PR 工作流](docs/agents/issue-tracker.md)选择交付方式、跟踪审核与集成；
新增记录前查阅[历史保留规则](docs/project-history/README.md)。
