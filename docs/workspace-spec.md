# Alexis Script & Plugin Package 工作区建立规范

> 目标仓库：`Alexis-Li/Alexis_Script_Plugin_Package`
> 默认分支：`main`
> 用途：统一管理 Maya 脚本、Maya 工具/插件，以及 Unreal Engine 插件。
> 执行对象：Codex

---

## 1. 任务目标

请将当前仓库建立为一个 **单仓库、多子项目（Monorepo）** 的 3D 美术工具开发工作区。

整个 `Alexis_Script_Plugin_Package` 仓库是唯一 Git 根目录。不要在任何子目录中再次执行 `git init`，不要创建嵌套 Git 仓库，也不要为每个插件单独创建 `.git` 文件夹。

仓库需要同时支持以下三类项目：

1. Maya 工具架单文件小脚本。
2. Maya 多文件正式工具或插件。
3. Unreal Engine 独立插件。

每个项目必须拥有清晰边界，能够独立维护；正式工具和插件应当能够独立安装、测试、打包和发布。

---

## 2. 仓库顶层结构

请建立以下目录和基础文件：

```text
Alexis_Script_Plugin_Package/
│
├─ .github/
│  ├─ workflows/
│  ├─ ISSUE_TEMPLATE/
│  └─ pull_request_template.md
│
├─ docs/
│  ├─ development-conventions.md
│  ├─ naming-conventions.md
│  ├─ maya-development.md
│  ├─ unreal-development.md
│  └─ release-process.md
│
├─ maya/
│  ├─ AGENTS.md
│  ├─ scripts/
│  └─ tools/
│
├─ unreal/
│  ├─ AGENTS.md
│  ├─ ToolsLab.uproject
│  ├─ Config/
│  ├─ Content/
│  ├─ Source/
│  └─ Plugins/
│
├─ templates/
│  ├─ maya-shelf-script/
│  ├─ maya-tool/
│  ├─ unreal-plugin/
│  └─ standalone-python-tool/
│
├─ tools/
│  ├─ create_project.py
│  ├─ validate_repository.py
│  ├─ package_maya_tool.py
│  ├─ package_unreal_plugin.py
│  └─ update_versions.py
│
├─ AGENTS.md
├─ README.md
├─ README_CN.md
├─ CHANGELOG.md
├─ LICENSE
├─ .gitignore
├─ .gitattributes
├─ .editorconfig
└─ pyproject.toml
```

没有实际内容的目录可以使用 `.gitkeep` 暂时保留。

不要生成 Unreal Engine 的缓存、编译结果、解决方案文件或 Maya 临时文件。

---

## 3. Maya 项目分类规则

Maya 项目必须先判断属于以下哪一类，再决定目录结构。

### 3.1 工具架单文件小脚本

放置位置：

```text
maya/scripts/<script-name>/
```

标准结构：

```text
<script-name>/
├─ <script_name>.py
├─ README.md
└─ README_CN.md
```

适用条件：

- 功能可以合理地保持在一个 Python 文件中。
- 完整代码可以复制到 Maya Script Editor 的 Python 标签页中直接运行。
- 可以将完整代码保存为 Maya 工具架按钮。
- 不需要安装流程。
- 不需要外部 Python 包、图标、配置文件、UI 文件或资源文件。
- 不包含自定义 Maya 节点、命令、文件转换器或编译插件。完全包含在单文件中、
  且以脚本编辑器或工具架直接运行作为预期工作流的 Context 可以保留为小脚本。
- 不需要启动项、后台服务、长期回调或持久化 Script Job。
- 不需要向其他工具提供稳定的公共 Python API。
- 单文件状态下依然容易阅读和维护。

允许使用：

- `maya.cmds`
- `maya.mel`
- `maya.api.OpenMaya`
- 简单窗口或短暂弹窗
- 短小明确的过程式逻辑

仅仅调用 Maya 命令，不代表必须升级为正式工具。

单文件脚本应提供明确的 `run()` 或 `main()` 入口。需要复制完整代码到 Maya
Script Editor 或工具架直接运行时，可以在文件末尾直接调用入口：

```python
def main():
    """Execute the script's main operation."""
    pass

main()
```

脚本不得依赖仓库根目录已加入 `PYTHONPATH`。复制完整代码到 Maya Script Editor 后必须能够直接运行。

工具架小脚本目录中不要添加：

- `package/`
- `src/`
- `scripts/`
- `core/`
- `ui/`
- `tests/`
- `docs/`
- `icons/`
- `.mod`
- `pyproject.toml`
- 独立 `AGENTS.md`
- 安装或卸载脚本

### 3.2 Maya 正式工具或插件

放置位置：

```text
maya/tools/<tool-name>/
```

满足以下任意条件时，应采用正式工具结构：

- 实现需要多个 Python 文件。
- 包含持久化 PySide 界面或复杂 Maya UI。
- 需要分离业务逻辑、Maya 场景操作和界面逻辑。
- 需要图标、预设、配置、模板或其他资源。
- 需要安装，不能单纯复制进 Script Editor。
- 使用自定义 Maya 节点、命令、Deformer、文件转换器或 Plug-in Manager。
- Context 需要安装、外部资源或多个功能模块，无法合理保持为可直接运行的单文件脚本。
- 注册长期回调、持久化 Script Job、菜单、工具架、启动 Hook 或事件监听器。
- 需要自动化测试。
- 向其他代码提供可复用 API。
- 需要明确版本、打包、升级或卸载流程。
- 单文件继续增长会降低可读性或维护安全性。

推荐结构：

```text
maya/tools/<tool-name>/
│
├─ README.md
├─ README_CN.md
├─ AGENTS.md
├─ CHANGELOG.md
├─ LICENSE
├─ pyproject.toml
│
├─ package/
│  ├─ <ToolName>.mod
│  └─ <ToolName>/
│     ├─ scripts/
│     │  └─ <python_package>/
│     │     ├─ __init__.py
│     │     ├─ bootstrap.py
│     │     ├─ constants.py
│     │     ├─ core/
│     │     ├─ maya/
│     │     ├─ ui/
│     │     └─ resources/
│     ├─ icons/
│     ├─ plug-ins/
│     └─ presets/
│
├─ tests/
│  ├─ unit/
│  ├─ maya/
│  └─ fixtures/
│
├─ docs/
│  ├─ architecture.md
│  ├─ installation.md
│  └─ development.md
│
└─ tools/
   ├─ install_dev.py
   ├─ uninstall_dev.py
   └─ package_release.py
```

不需要的目录可以删除，不要为了形式创建空架构。

Maya 模块文件示例：

```text
+ ToolName 0.1.0 ./ToolName
```

代码职责：

```text
core/       不依赖 UI 的业务逻辑
maya/       maya.cmds、OpenMaya、场景和选择操作
ui/         PySide 窗口、控件和用户交互
resources/  配置、样式和非标准资源
bootstrap.py 对外唯一启动入口
```

推荐公共启动方式：

```python
from package_name import bootstrap
bootstrap.show()
```

不要要求用户直接实例化内部窗口类。

### 3.3 重新分类规则

始终从满足需求的最小结构开始。

工具架小脚本出现以下任意情况时，应移动到 `maya/tools/` 并升级为正式工具：

- 出现第二个功能性 Python 模块。
- 增加持久化或复杂 UI。
- 增加外部资源或配置。
- 需要安装流程。
- 需要可复用公共 API。
- 注册长期回调或启动行为。
- 增加自定义插件功能，或 Context 已无法合理保持在单文件中。
- 已经需要单独测试或明确架构边界。

转换时：

1. 将项目从 `maya/scripts/` 移动到 `maya/tools/`。
2. 尽量保留原有用户操作方式。
3. 提供明确的公共入口。
4. 增加安装和使用说明。
5. 按实际需要分离 UI、Maya 集成和核心逻辑。

---

## 4. Maya 工具架脚本 README 模板

每个工具架脚本提供英文 `README.md` 和内容对应的中文 `README_CN.md`。
两份文件都面向使用者，只保留简介、安装方式和使用方式：

```markdown
# Script Name

## Introduction

说明脚本解决什么问题、适用版本和运行前需要的选择或场景状态。

## Installation

1. 打开 Maya Script Editor。
2. 创建 Python 标签页。
3. 复制 `<script_name>.py` 的完整内容。
4. 选中全部代码。
5. 使用鼠标中键拖到 Maya 工具架。

## Usage

1. 选择目标对象。
2. 点击工具架按钮。

```

需要给开发者看的场景修改、撤销实现、限制和维护说明放入 `docs/` 或
`AGENTS.md`，不要增加到面向用户的 README 中。

单文件脚本应：

- 对输入进行验证。
- 使用明确、可执行的 Maya Warning 或 Error。
- 多步场景修改尽量放入一个 Maya Undo Chunk。
- 不修改用户首选项或系统环境。
- 不遗留临时节点、命名空间、回调或无意的选择变化。
- 对无法撤销的操作明确提示。

---

## 5. Unreal Engine 插件规范

所有 Unreal 插件放在：

```text
unreal/Plugins/<PluginName>/
```

推荐结构：

```text
unreal/Plugins/<PluginName>/
│
├─ <PluginName>.uplugin
├─ README.md
├─ README_CN.md
├─ AGENTS.md
├─ CHANGELOG.md
├─ LICENSE
│
├─ Config/
├─ Content/
├─ Resources/
│  └─ Icon128.png
│
├─ Source/
│  ├─ <PluginName>/
│  │  ├─ <PluginName>.Build.cs
│  │  ├─ Public/
│  │  └─ Private/
│  │
│  ├─ <PluginName>Editor/
│  │  ├─ <PluginName>Editor.Build.cs
│  │  ├─ Public/
│  │  └─ Private/
│  │
│  └─ <PluginName>Tests/
│
├─ Docs/
└─ Scripts/
```

规则：

- 每个 `Plugins/` 直属子目录必须是可独立识别的 UE 插件。
- `.uplugin` 必须位于插件根目录。
- Runtime 功能放 Runtime 模块。
- 菜单、编辑器 UI、资产操作和编辑器工具放 Editor 模块。
- 公共头文件只暴露真正需要的公共 API。
- 实现细节放在 `Private/`。
- 插件资源必须放在插件自己的 `Content/`，不要放入测试工程主 `Content/`。
- 没有资产时删除 `Content/`，并将 `CanContainContent` 设为 `false`。
- 测试工程 `ToolsLab` 只负责加载和验证插件，不承载可复用插件代码。
- 不提交 `Binaries`、`Intermediate`、`Saved`、`DerivedDataCache` 和 `.vs`。

---

## 6. README 分层规则

### 6.1 双语文件

仓库根目录以及每个面向用户的脚本、工具或插件都提供两份对应说明：

- `README.md`：英文版。
- `README_CN.md`：中文版。

两份说明提供互相跳转的链接，并保持信息一致。

### 6.2 内容范围

README 只面向使用者，保持简单并只包含：

1. 简介：用途、关键功能以及必要的兼容信息。
2. 安装方式：用户获得可运行工具所需的步骤。
3. 使用方式：启动工具和完成主要操作的步骤。

目录结构、架构、开发命令、测试、打包、发布和 Codex 行为规则放入
`docs/`、`AGENTS.md` 或 `CHANGELOG.md`，不要写入用户 README。

---

## 7. AGENTS.md 层级

建议使用以下层级：

```text
AGENTS.md
maya/AGENTS.md
unreal/AGENTS.md
特殊复杂项目/AGENTS.md
```

规则：

- 根 `AGENTS.md`：全仓库通用规则。
- `maya/AGENTS.md`：Maya 专用分类和开发规则。
- `unreal/AGENTS.md`：UE 插件专用规则。
- 只有确实存在特殊约束的复杂项目才增加项目级 `AGENTS.md`。
- 不要在多个文件中重复相同规则。

本机路径和个人配置写入：

```text
AGENTS.override.md
```

并加入 `.gitignore`，不要上传 GitHub。

---

## 8. 根 AGENTS.md 内容

请创建以下根规则：

```markdown
# Repository Instructions

## Repository Purpose

This repository is a monorepo containing independent Autodesk Maya scripts,
Maya tools, and Unreal Engine plugins for 3D art production.

The repository root is the only Git repository. Do not create nested Git
repositories under any project directory.

Every structured tool or plugin must remain independently installable,
testable, versioned, and packageable.

## Repository Layout

- `maya/scripts/`: self-contained Maya shelf scripts
- `maya/tools/`: structured Maya tools and plugins
- `unreal/Plugins/`: standalone Unreal Engine plugins
- `templates/`: project templates
- `tools/`: repository-level validation and packaging scripts
- `docs/`: shared development documentation

## General Rules

- Make the smallest coherent change required by the task.
- Do not refactor unrelated code.
- Do not rename public APIs without explicit approval.
- Do not add machine-specific absolute paths.
- Do not commit secrets, credentials, personal paths, caches, or generated files.
- Do not modify third-party code unless explicitly required.
- Preserve backward compatibility unless a breaking change is requested.
- Update documentation when installation, public APIs, supported versions,
  directory structures, or user-visible behavior change.
- Do not over-engineer simple Maya shelf scripts.
- Keep user-facing `README.md` files in English and pair them with a Chinese
  `README_CN.md` containing the same information.
- Limit user-facing READMEs to introduction, installation, and usage.

## Naming Conventions

- Directories and repositories: lowercase kebab-case
- Python packages and modules: lowercase snake_case
- Python classes: PascalCase
- Unreal plugins and modules: PascalCase
- Unreal C++ classes: Unreal Engine naming conventions
- Public versions: Semantic Versioning where practical

## Project Boundaries

Each structured project owns its:

- README.md
- README_CN.md
- CHANGELOG.md
- version information
- tests
- documentation
- packaging scripts
- runtime dependencies

Sibling projects must not depend on each other unless the dependency is
explicitly documented and independently packageable.

## Generated Files

Never commit generated or machine-specific files, including:

- Python bytecode and caches
- Maya crash logs and temporary files
- Unreal `Binaries`
- Unreal `Intermediate`
- Unreal `Saved`
- Unreal `DerivedDataCache`
- Visual Studio generated files
- packaged release archives

Release archives belong in GitHub Releases, not Git history.

## Validation

Before considering a task complete:

1. Run the narrowest relevant tests.
2. Run repository structural validation.
3. Confirm no generated files were added.
4. Confirm documentation remains accurate.
5. Report tests that could not be run and why.

## Git Rules

- Do not initialize nested repositories.
- Do not force-push.
- Do not use destructive reset operations.
- Do not rewrite unrelated history.
- Keep commits scoped to one logical change.
- Use project-prefixed release tags.

Examples:

- `maya-freeze-pivots-v0.1.0`
- `maya-mesh-normal-tool-v0.2.0`
- `ue-asset-audit-v1.1.0`
```

---

## 9. maya/AGENTS.md 内容

将第 3 节“ Maya 项目分类规则”完整写入 `maya/AGENTS.md`，并增加以下规则：

```markdown
## Python Structure

For structured Maya tools:

- Keep business logic separate from Maya scene access and UI code.
- `core/` must not import UI modules.
- `ui/` may call `core/`, but `core/` must not call `ui/`.
- Maya-specific scene access belongs in `maya/`.
- Provide one documented public entry point through `bootstrap.py`.
- Avoid executing Maya commands during module import.

## Maya Compatibility

- Do not assume a system Python installation.
- Use the Python and Qt versions shipped with supported Maya versions.
- Do not introduce PyMEL unless explicitly required.
- Do not hardcode Maya installation paths.
- Guard version-specific APIs explicitly.

## UI

- Parent Maya windows correctly.
- Repeated launches must not create duplicate windows.
- Clean up callbacks, Script Jobs, and event handlers when windows close.
- Keep user-visible errors actionable and concise.

## Tests

- Pure Python logic should be testable outside Maya where practical.
- Maya integration tests must be separate from unit tests.
- Tests must not modify user preferences or production scenes.
```

---

## 10. unreal/AGENTS.md 内容

```markdown
# Unreal Engine Development Instructions

These instructions apply to the Unreal Engine test project and all plugins
under `unreal/Plugins/`.

## Plugin Boundaries

- Every directory directly under `Plugins/` must be a valid standalone plugin.
- The `.uplugin` file must remain at the plugin root.
- Dependencies must be declared in `.uplugin` and `.Build.cs`.
- Do not depend on files outside the plugin unless explicitly documented.
- Do not place reusable plugin code in the ToolsLab host project.

## Module Structure

- Runtime code belongs in Runtime modules.
- Editor UI, menus, asset actions, and editor utilities belong in Editor modules.
- Public headers expose only intentional public APIs.
- Implementation details belong in `Private/`.
- Avoid unnecessary `PublicDependencyModuleNames`.

## Generated Files

Do not commit:

- `Binaries/`
- `Intermediate/`
- `Saved/`
- `DerivedDataCache/`
- `.vs/`
- generated solution and project files

## Assets

- Keep plugin assets inside the plugin's own `Content/`.
- Do not place plugin assets in the host project's main `Content/`.
- Use Git LFS for Unreal binary assets.
- Do not add large samples without documenting their purpose.

## Validation

After code changes:

1. Compile affected modules.
2. Check for new warnings.
3. Run relevant automation tests.
4. Verify the plugin loads in ToolsLab.
5. Confirm no generated folders were staged.
```

---

## 11. .gitignore

请建立：

```gitignore
# Local agent instructions
AGENTS.override.md

# Operating system
.DS_Store
Thumbs.db
Desktop.ini

# Editors
.vscode/
.idea/
*.code-workspace

# Python
__pycache__/
*.py[cod]
*.pyd
.pytest_cache/
.mypy_cache/
.ruff_cache/
.coverage
htmlcov/
.venv/
venv/
build/
dist/
*.egg-info/

# Maya
mayaCrashLog*
mayaRenderLog.txt
*.swatches
*.autosave
*.tmp
*.bak

# Unreal Engine
Binaries/
Build/
DerivedDataCache/
Intermediate/
Saved/
.vs/
*.sln
*.suo
*.opensdf
*.sdf
*.VC.db
*.VC.opendb

# Packaged output
releases/
artifacts/
unreal/Packaged/
unreal/Releases/
*.zip
*.7z
```

不要忽略：

```text
Content/
Config/
Resources/
Source/
*.uplugin
*.uproject
```

---

## 12. .gitattributes 和 Git LFS

请建立：

```gitattributes
* text=auto

*.py text eol=lf
*.md text eol=lf
*.json text eol=lf
*.ini text eol=lf
*.uplugin text eol=lf
*.uproject text eol=lf
*.cs text eol=lf
*.cpp text eol=lf
*.h text eol=lf

*.uasset filter=lfs diff=lfs merge=lfs -text
*.umap filter=lfs diff=lfs merge=lfs -text
*.fbx filter=lfs diff=lfs merge=lfs -text
*.abc filter=lfs diff=lfs merge=lfs -text
*.usd filter=lfs diff=lfs merge=lfs -text
*.usdc filter=lfs diff=lfs merge=lfs -text
*.mb filter=lfs diff=lfs merge=lfs -text
*.psd filter=lfs diff=lfs merge=lfs -text
*.exr filter=lfs diff=lfs merge=lfs -text
*.tif filter=lfs diff=lfs merge=lfs -text
*.tiff filter=lfs diff=lfs merge=lfs -text
```

不要默认将普通 `.png`、`.jpg` 和 `.svg` 全部放入 LFS。

---

## 13. 版本与发布规则

每个正式项目独立维护版本。

Git 标签格式：

```text
maya-<project-name>-v<version>
ue-<plugin-name>-v<version>
```

示例：

```text
maya-mesh-normal-tool-v0.1.0
maya-skin-weight-tool-v1.2.0
ue-asset-audit-v0.4.0
```

发布文件示例：

```text
MeshNormalTool-0.1.0.zip
AssetAudit-0.4.0-UE5.7.zip
```

发布压缩包上传到 GitHub Releases，不要提交进仓库历史。

---

## 14. 模板要求

### 14.1 maya-shelf-script 模板

```text
templates/maya-shelf-script/
├─ script_name.py
├─ README.md
└─ README_CN.md
```

### 14.2 maya-tool 模板

采用本规范第 3.2 节的正式 Maya 工具结构，保留最小可运行示例：

- Python 包。
- `bootstrap.py`。
- 最小 `.mod` 文件。
- `README.md` 和 `README_CN.md`。
- CHANGELOG。
- 测试占位。
- 安装和打包脚本占位。

### 14.3 unreal-plugin 模板

提供最小 Editor 插件模板：

- `.uplugin`
- `Source/<PluginName>Editor`
- `Build.cs`
- 模块头文件和实现文件
- `Resources/Icon128.png` 占位说明
- `README.md` 和 `README_CN.md`
- CHANGELOG
- AGENTS

### 14.4 standalone-python-tool 模板

提供最小可运行的 Python 包、测试占位，以及 `README.md` 和
`README_CN.md` 两份用户说明。

---

## 15. 工作区建立时的执行边界

本次只建立基础工作区和规范文件，不要：

- 创建无需求支撑的大量示例业务代码。
- 编造现有插件。
- 生成完整 Unreal 二进制插件。
- 下载大型第三方依赖。
- 把本机绝对路径写进仓库。
- 修改 Git 远程地址。
- 创建子仓库。
- 删除仓库中已有且用途不明的文件。
- 自动推送到远程，除非用户明确要求。
- 自动创建 GitHub Release。

如仓库已有文件，先检查并保留有价值内容，再进行增量调整。

---

## 16. 验收清单

完成后确认：

- [ ] 仓库根目录是唯一 Git 根目录。
- [ ] 已创建 `maya/scripts` 和 `maya/tools`。
- [ ] 已创建 `unreal/Plugins` 和最小测试工程目录。
- [ ] 已创建根 `AGENTS.md`。
- [ ] 已创建 `maya/AGENTS.md`。
- [ ] 已创建 `unreal/AGENTS.md`。
- [ ] 已创建根 `README.md`。
- [ ] 已创建根 `README_CN.md`。
- [ ] 已创建 `.gitignore`。
- [ ] 已创建 `.gitattributes`。
- [ ] 已创建 `.editorconfig`。
- [ ] 已创建 Maya 小脚本模板。
- [ ] 已创建 Maya 正式工具模板。
- [ ] 已创建 UE 插件模板。
- [ ] 未创建嵌套 `.git`。
- [ ] 未提交生成文件和缓存。
- [ ] 未写入本机绝对路径。
- [ ] 仓库结构验证脚本能够检查项目分类和禁止文件。
- [ ] `git status` 中只包含预期的源码和规范文件。

---

## 17. Codex 最终汇报格式

执行完成后，请给出：

1. 创建和修改的文件列表。
2. 最终目录树。
3. 项目分类规则的简要说明。
4. 已运行的验证命令和结果。
5. 未运行的验证以及原因。
6. 当前 `git status` 摘要。
7. 下一步建议，但不要自动提交或推送，除非用户明确要求。
