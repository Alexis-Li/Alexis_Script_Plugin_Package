# Alexis 脚本与插件合集

[English](README.md)

## 这个仓库是什么

这是一个面向 3D 美术生产的 Autodesk Maya 脚本与工具合集，每个项目都可以
独立使用；仓库同时提供独立 Unreal Engine 插件的开发工作区。

Maya 项目优先提供可直接运行的单个 Python 文件。只有功能确实需要时，才增加
`scripts/`、`plug-ins/`、`icons/` 或其他文件。中小型工具通过将这些文件复制到
Maya 对应目录安装，不要求提供 `.mod` 文件。Maya 运行代码应尽量同时兼容 Python
2 和 Python 3；无法兼容时优先支持 Python 3。

## 包含的工具

| 工具 | 用途 | 位置 |
| --- | --- | --- |
| Flatten Mesh To UV | 将模型的 UV 展开结果转换为平面多边形。 | [`maya/tools/FlattenMeshToUV`](maya/tools/FlattenMeshToUV/) |
| NitroPoly | 提供选择、拓扑、轴心、连接和循环边等多边形建模功能。 | [`maya/tools/NitroPoly`](maya/tools/NitroPoly/) |
| ZiSpread | 通过交互拖动均匀分布所选环线。 | [`maya/scripts/ZiSpread`](maya/scripts/ZiSpread/) |

## 工具文档

- [Flatten Mesh To UV](maya/tools/FlattenMeshToUV/README_CN.md)
- [NitroPoly](maya/tools/NitroPoly/README_CN.md)
- [ZiSpread](maya/scripts/ZiSpread/README_CN.md)
- [Maya 开发规则](docs/maya-development.md)
- [命名规范](docs/naming-conventions.md)
- [发布流程](docs/release-process.md)

## 开始开发

1. 安装 Python 3.10 或更高版本，用于运行仓库维护命令。
2. 修改前先阅读 [`AGENTS.md`](AGENTS.md) 和对应平台目录内的 `AGENTS.md`。
3. 先预览项目模板，确认无误后再加 `--apply` 创建文件：

   ```powershell
   python tools/create_project.py maya-script SampleTool --json
   ```

4. Maya 用户直接运行的项目目录和入口文件使用 PascalCase，例如
   `FlattenMeshToUV` 和 `FlattenMeshToUV.py`。
5. 交付修改前运行：

   ```powershell
   python -m unittest discover -s tests
   python tools/validate_repository.py
   ```

通用流程见[开发约定](docs/development-conventions.md)，Unreal 规则见
[Unreal 开发说明](docs/unreal-development.md)。
