# Alexis 脚本与插件合集

[English](README.md)

## 这个仓库是什么

这是一个面向 3D 游戏美术生产的脚本、工具与插件合集，每个项目都可以独立使用。
仓库覆盖 Autodesk Maya、Unreal Engine 和 Adobe Substance 3D Painter，也包括在
这些软件之间协作的复合产品。

从下表选择工具，按对应手册确认支持的软件版本、安装方式和使用步骤。
各工具及宿主组件均可独立安装。
合集同时管理本仓库项目和独立子模块；独立项目的源码、工单与发布由其自己的仓库管理。

## 包含的工具

| 工具 | 用途 | 位置 |
| --- | --- | --- |
| Flatten Mesh To UV | 将模型的 UV 展开结果转换为平面多边形。 | [`maya/tools/FlattenMeshToUV`](maya/tools/FlattenMeshToUV/) |
| MtoU Live Link | 通过可独立安装的 Maya 和 Unreal 组件，在 Unreal Editor 中实时或按缓存预览 Maya 角色动画，支持镜头联动、副对象和场景参考。 | [`composite/MtoULiveLink`](composite/MtoULiveLink/) |
| NitroPoly | 提供选择、拓扑、轴心、连接和循环边等多边形建模功能。 | [`maya/tools/NitroPoly`](maya/tools/NitroPoly/) |
| StoUTextureSync | Painter 到 Unreal 的贴图同步项目，处于原型阶段。 | [独立仓库](https://github.com/Alexis-Li/StoUTextureSync) · 子模块 `substance-painter/plugins/StoUTextureSync` |
| ZiSpread | 通过交互拖动均匀分布所选环线。 | [`maya/scripts/ZiSpread`](maya/scripts/ZiSpread/) |

## 工具文档

- [Flatten Mesh To UV](maya/tools/FlattenMeshToUV/README_CN.md)
- [MtoU Live Link](composite/MtoULiveLink/README_CN.md)
- [NitroPoly](maya/tools/NitroPoly/README_CN.md)
- [ZiSpread](maya/scripts/ZiSpread/README_CN.md)
- [仓库地图与支持的宿主软件](docs/workspace-spec.md)
- [Maya 项目指南](docs/maya-development.md)
- [命名规范](docs/naming-conventions.md)
- [发布流程](docs/release-process.md)
- [设计中的产品](docs/proposals/README.md)
- [项目开发历史](docs/project-history/README.md)

## 获取独立项目

首次克隆使用 `git clone --recurse-submodules <仓库地址>`；已有工作区在确认子模块
没有待保存修改后，运行 `git submodule update --init --recursive`。开发、更新版本引用
及迁移步骤见[子模块工作流](docs/submodules.md)。

## 开始开发

1. 安装 Python 3.10 或更高版本，用于运行仓库维护命令。
2. 修改前先阅读 [`AGENTS.md`](AGENTS.md) 和对应宿主的规则文件（`maya/`、
   `unreal/` 或 `substance-painter/AGENTS.md`）。
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
