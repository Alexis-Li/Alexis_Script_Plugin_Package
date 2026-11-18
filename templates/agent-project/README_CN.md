# Agent 项目模板

与宿主软件无关的文档和规则骨架，用于新建由 AI Agent 开发、服务于不写代码的
3D 美术用户的独立项目。它收录现有独立项目（LoomlineRetopo、MtoULiveLink、
StoUTextureSync）共有的 Agent 工作流：给 Agent 的规则、从需求到开发线路的
文档结构、实现与审核分离、Issue 与 PR 约定、会话交接和变更记录约定。

模板是起点。把 `skeleton/` 复制到新仓库的根目录，新项目再按自己的产品修改。

## 骨架包含什么

下表的文件名是新仓库里的名字。`skeleton/` 中带 `.tmpl` 后缀的文件在创建时
改名：集合仓库里若出现真正的 `AGENTS.md`、`.gitignore`、`.gitattributes`
或 `.editorconfig`，它们会被当作这个目录的现行规则。

| 文件 | 负责什么 |
| --- | --- |
| `AGENTS.md`（来自 `AGENTS.md.tmpl`） | 给 Agent 的规则：阅读顺序、与用户配合、审核强制执行的架构规则、环境、默认检查、数据与隐私、会话交接、文档与交付、版本 |
| `CONTEXT.md` | 术语表：中文、代码名、含义 |
| `docs/workflow.md` | 交付流程：实现与审核分开、什么问用户、分支、PR、审核、实验任务、目视验收、架构变更、样本、Issue 评论、发布 |
| `docs/requirements.md` | 需求基线，编号为 `G-`、`F-`、`NG-`、`Q-` |
| `docs/roadmap.md` | 阶段与任务（`S<阶段>-T<n>`），写明范围、完成标准和是否需要用户 |
| `docs/architecture.md` | 结构、模块边界和架构决定（`AD-n`） |
| `docs/adr/README.md` | 何时、怎么写 ADR，附空模板 |
| `README.md`、`README_CN.md` | 新项目的用户手册，英文与中文 |
| `CHANGELOG.md` | 用户可见的变化，记在 `## Unreleased` 下 |
| `.github/pull_request_template.md` | PR 描述：范围、SHA、验证、遗留 |
| `.github/ISSUE_TEMPLATE/` | 缺陷报告、任务（子工单）、阶段（总工单）表单 |
| `.gitignore`、`.gitattributes`、`.editorconfig`（来自 `*.tmpl`） | 通用的忽略、换行和编辑器规则 |

骨架里的 `tools/`（项目校验、状态评论脚本）、`tests/` 和
`.github/workflows/`（CI）由新项目自己的文档说明。

## 创建项目

由 Agent 用集合仓库的创建脚本生成。脚本先预览，加 `--apply` 才写入：

```powershell
python tools/create_agent_project.py <ProjectName> --destination <path-to-new-repo>
python tools/create_agent_project.py <ProjectName> --destination <path-to-new-repo> --apply
```

`<ProjectName>` 用 PascalCase（例如 `LoomlineRetopo`），替换所有
`{{PROJECT_NAME}}`。生成之后，在新仓库里 `git init`，提交骨架，并建立第一个
开发分支 `dev/<主题>`。

## 首次搭建

第一个功能任务开始之前，逐项做完：

1. 处理每一个 `待定制：` 标记。标记写明要填什么，并给出现有项目的例子；
   填好后连标记一起删掉。整节不适用的，连同标记删除整节。只要还有标记，
   `python tools/validate_project.py` 就会报错。
2. 与用户商定方向，把产品的实际内容填进 `CONTEXT.md`、`docs/architecture.md`、
   `docs/requirements.md` 和 `docs/roadmap.md`。
3. 确定检查命令，写进 `AGENTS.md` 的默认检查表；CI 里跑能跑的那些。
4. 在 GitHub 上开启 **Automatically delete head branches**。仓库是否公开、
   用什么许可证由用户决定，Agent 不替用户选。
5. 运行 `python tools/validate_project.py` 和
   `python -m unittest discover -s tests`，直到都通过。

## 哪些保留，哪些改

下面这些是通用核心，除非用户另有决定，建议保留：实现与审核分离和 PR 规则、
会话交接、评论和状态评论规则、分支模型、信息归属表、数据与隐私规则、
变更记录约定。

按项目修改：产品描述、阅读顺序里的行、架构规则、环境和宿主版本、检查命令、
样本的定义、目视验收包（产品没有需要用户看的产出时，删去 `docs/workflow.md`
第 7 节）、发布与打包命令。

两种可选做法：

- 需求放在 Issue 里，不用 `docs/requirements.md`：由一个产品 Issue 持有需求，
  子工单引用它的编号（StoUTextureSync 的做法）。删除 `docs/requirements.md`，
  把阅读顺序、`docs/roadmap.md` 和任务模板里的指向改到那个 Issue。
- 长期历史记录：项目需要在 ADR 之外保留选定的决定和证据时，增加
  `docs/history/` 并写明保留规则，MtoULiveLink 和 StoUTextureSync 就是这样做的。
  只在某条记录没有更合适的存放处时才加。

## 维护模板

模板的改动不会自动同步到已经建好的项目。项目里验证过的、属于共享核心的
改进，回写到 `skeleton/`，项目特有的部分仍留作标记。骨架里不出现任何真实
项目的术语、路径和样本名。
