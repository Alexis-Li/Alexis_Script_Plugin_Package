# Composite Plugin Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Add a governed `composite/` repository category and migrate MtoU_LiveLink into independently copyable Maya and Unreal component roots without changing its runtime behavior or CLI packaging contracts.

**Architecture:** Component lookup is centralized in `tools/_repo_tools.py`, where each packager resolves exactly one legacy or composite location. Repository validation reuses host-specific validators for legacy and composite roots, while ToolsLab discovers the moved Unreal plugin through `AdditionalPluginDirectories`.

**Tech Stack:** Python 3 standard library, `unittest`, Ruff, Autodesk Maya 2022.4 `mayapy`, Unreal Engine 5.7.4 build and Automation tests, Git.

## Global Constraints

- Target layout: `composite/MtoULiveLink/maya/MtoULiveLink/` and `composite/MtoULiveLink/unreal/MtoULiveLink/`.
- The nested host component directory is the independently installable and packageable root.
- Keep `MtoULiveLink`, `MtoULiveLinkEditor`, `MtoULiveLink.uplugin`, `FriendlyName`, protocol, endpoint, subject, and version unchanged.
- Preserve the CLI contracts `python tools/package_maya_tool.py MtoULiveLink --json` and `python tools/package_unreal_plugin.py MtoULiveLink --engine 5.7 --json`.
- Exactly one legacy or composite component location may exist; two matches are an actionable ambiguity error.
- Keep existing single-host projects at `maya/tools/` and `unreal/Plugins/` working unchanged.
- Support stock Maya 2022.4 and stock Unreal Engine 5.7.4; do not claim compatibility with the modified Unreal build.
- Add no production dependency and no composite template.
- Keep English and Chinese user READMEs paired and aligned.
- Do not commit generated Maya, Unreal, Python, IDE, or packaged archive output.

---

### Task 1: Resolve Legacy and Composite Components

**Files:**
- Modify: `tests/test_repository_tools.py`
- Modify: `tools/_repo_tools.py`
- Modify: `tools/package_maya_tool.py`
- Modify: `tools/package_unreal_plugin.py`
- Move: `maya/tools/MtoULiveLink/` to `composite/MtoULiveLink/maya/MtoULiveLink/`
- Move: `unreal/Plugins/MtoULiveLink/` to `composite/MtoULiveLink/unreal/MtoULiveLink/`

**Interfaces:**
- Consumes: repository root `ROOT` and PascalCase project name validation.
- Produces: `resolve_component(label: str, candidates: tuple[Path, ...]) -> Path`, `maya_tool_path(name: str) -> Path`, and `unreal_plugin_path(name: str) -> Path`.

- [x] **Step 1: Write failing resolver and location tests**

Add imports for `_repo_tools` and `package_unreal_plugin`, then add tests equivalent to:

```python
def test_component_resolver_rejects_ambiguous_locations(self):
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        first = root / "legacy" / "MtoULiveLink"
        second = root / "composite" / "MtoULiveLink"
        first.mkdir(parents=True)
        second.mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, "ambiguous Maya tool"):
            _repo_tools.resolve_component("Maya tool", (first, second))

def test_mtou_components_resolve_from_composite_project(self):
    self.assertEqual(
        pathlib.Path("composite/MtoULiveLink/maya/MtoULiveLink"),
        _repo_tools.maya_tool_path("MtoULiveLink").relative_to(ROOT),
    )
    self.assertEqual(
        pathlib.Path("composite/MtoULiveLink/unreal/MtoULiveLink"),
        _repo_tools.unreal_plugin_path("MtoULiveLink").relative_to(ROOT),
    )
```

- [x] **Step 2: Run the targeted tests and verify RED**

Run:

```powershell
python -m unittest tests.test_repository_tools.RepositoryToolTests.test_component_resolver_rejects_ambiguous_locations tests.test_repository_tools.RepositoryToolTests.test_mtou_components_resolve_from_composite_project -v
```

Expected: errors because the three resolver functions do not exist.

- [x] **Step 3: Move both component roots with Git history**

Create the two lowercase host parent directories and use `git mv` for the complete existing component directories. Confirm the old roots are absent, the composite root owns the README pair, changelog, and license, and each destination contains only its runtime source, nearby tests, and descriptor where applicable.

- [x] **Step 4: Implement exact-one component resolution**

Add this behavior to `tools/_repo_tools.py`:

```python
def resolve_component(label: str, candidates: tuple[Path, ...]) -> Path:
    matches = [path for path in candidates if path.is_dir()]
    if not matches:
        raise ValueError("{0} not found".format(label))
    if len(matches) > 1:
        locations = ", ".join(relative(path) for path in matches)
        raise ValueError("ambiguous {0}: {1}".format(label, locations))
    return matches[0]

def maya_tool_path(name: str) -> Path:
    name = require_pascal_name(name)
    return resolve_component(
        "Maya tool",
        (ROOT / "maya" / "tools" / name, ROOT / "composite" / name / "maya" / name),
    )

def unreal_plugin_path(name: str) -> Path:
    name = require_pascal_name(name)
    return resolve_component(
        "Unreal plugin",
        (ROOT / "unreal" / "Plugins" / name, ROOT / "composite" / name / "unreal" / name),
    )
```

Make the Maya packager call `maya_tool_path(name)`. Make the Unreal packager call `unreal_plugin_path(plugin_name)` and report a descriptor error using the resolved repository-relative location.

- [x] **Step 5: Run resolver and packaging tests and verify GREEN**

Run:

```powershell
python -m unittest tests.test_repository_tools -v
python tools/package_maya_tool.py MtoULiveLink --json
python tools/package_unreal_plugin.py MtoULiveLink --engine 5.7 --json
```

Expected: all repository-tool tests pass; both dry-runs report `ok: true`, unchanged archive versions, and non-zero file counts.

- [x] **Step 6: Commit the component move and resolver behavior**

```powershell
git add tools tests composite/MtoULiveLink maya/tools/MtoULiveLink unreal/Plugins/MtoULiveLink
git commit -m "refactor(mtou): move components into composite layout"
```

### Task 2: Validate Composite Projects and Git Worktrees

**Files:**
- Create: `composite/AGENTS.md`
- Create: `composite/MtoULiveLink/README.md`
- Create: `composite/MtoULiveLink/README_CN.md`
- Create: `composite/MtoULiveLink/CHANGELOG.md`
- Create: `composite/MtoULiveLink/LICENSE`
- Modify: `tests/test_repository_tools.py`
- Modify: `tools/validate_repository.py`

**Interfaces:**
- Consumes: `maya_version(project: Path) -> str` and host component roots created in Task 1.
- Produces: `_validate_maya_project(project: Path, errors: list[str], root: Path) -> None`, `_validate_unreal_plugin(plugin: Path, errors: list[str], root: Path) -> None`, `_validate_composite(root: Path, errors: list[str]) -> None`, and `_validate_nested_git(root: Path, errors: list[str]) -> None`.

- [x] **Step 1: Write failing composite and worktree tests**

Add a temporary composite fixture that lacks root metadata, invoke `_validate_composite`, and assert errors for `README.md`, `README_CN.md`, `CHANGELOG.md`, and `LICENSE`. Add another temporary fixture with a nested `.git` file and a nested `.git` directory, invoke `_validate_nested_git`, and assert only the directory is reported.

```python
def test_git_worktree_file_is_not_a_nested_repository(self):
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        (root / ".git").mkdir()
        worktree_git = root / ".worktrees" / "feature" / ".git"
        worktree_git.parent.mkdir(parents=True)
        worktree_git.write_text("gitdir: elsewhere", encoding="utf-8")
        nested_git = root / "vendor" / ".git"
        nested_git.mkdir(parents=True)
        errors = []
        validate_repository._validate_nested_git(root, errors)
        self.assertEqual(["nested Git repository: vendor/.git"], errors)
```

- [x] **Step 2: Run the targeted tests and verify RED**

Run the new test methods directly with `python -m unittest ... -v`.

Expected: errors because `_validate_composite` and `_validate_nested_git` do not exist.

- [x] **Step 3: Add composite governance and product metadata**

`composite/AGENTS.md` must require independently installable host roots, lowercase host directory names, PascalCase project/component names, root-owned paired READMEs, and no implicit cross-host filesystem dependency. The bilingual product READMEs must describe supported Maya 2022.4 and stock UE 5.7.4 and show the exact component copy destinations. The product changelog records the layout migration; the product license matches the repository license.

- [x] **Step 4: Extract reusable host validators and add composite validation**

Refactor current Maya and Unreal loops into the interfaces above. Validate every direct child of `composite/` as PascalCase with four root metadata files and at least two lowercase host directories. For `maya/<ProjectName>` call `_validate_maya_project`; for `unreal/<PluginName>` call `_validate_unreal_plugin`. Add `composite/AGENTS.md` to `REQUIRED_PATHS`.

`_validate_nested_git` must report nested `.git` directories only; a `.git` file is a registered worktree marker, not a nested repository.

- [x] **Step 5: Run tests and structural validation**

```powershell
python -m unittest tests.test_repository_tools -v
python tools/validate_repository.py --json
```

Expected: all tests pass and the validator returns `ok: true` without treating `.worktrees/mtou-livelink/.git` as a nested repository.

- [x] **Step 6: Commit validation and governance**

```powershell
git add composite tools/validate_repository.py tests/test_repository_tools.py
git commit -m "feat(repo): validate composite plugins"
```

### Task 3: Wire ToolsLab and Synchronize Current Documentation

**Files:**
- Modify: `unreal/ToolsLab.uproject`
- Modify: `AGENTS.md`
- Modify: `README.md`
- Modify: `README_CN.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/workspace-spec.md`
- Modify: `docs/naming-conventions.md`
- Modify: `docs/development-conventions.md`
- Modify: `docs/maya-development.md`
- Modify: `docs/unreal-development.md`
- Modify: `docs/release-process.md`
- Modify: `docs/superpowers/specs/2026-07-31-mtou-livelink-design.md`
- Modify: `docs/superpowers/plans/2026-07-31-mtou-livelink.md`
- Modify: `docs/mtou-livelink-stock-acceptance-2026-08-11.md`

**Interfaces:**
- Consumes: composite roots and packaging/validation behavior from Tasks 1-2.
- Produces: ToolsLab external plugin discovery and one consistent current documentation model.

- [x] **Step 1: Add ToolsLab external plugin discovery**

Keep the existing enabled plugin entry and add exactly:

```json
"AdditionalPluginDirectories": [
  "../composite/MtoULiveLink/unreal"
]
```

Validate the JSON immediately with `python -m json.tool unreal/ToolsLab.uproject`.

- [x] **Step 2: Update repository governance and indexes**

Document `composite/<ProjectName>/<host>/<ComponentName>/` in root governance, workspace, naming, development, host-specific, and release documents. List MtoU_LiveLink once in both root READMEs and link to the composite bilingual README. Add an Unreleased changelog entry for the migration.

- [x] **Step 3: Update active MtoU path references**

In the current MtoU design, implementation plan, and stock acceptance record, replace active references to the old Maya and Unreal roots with the new composite component roots. Keep the two old roots only in the approved migration design section that explicitly says they are pre-migration paths that must be absent.

- [x] **Step 4: Scan documentation and verify tooling**

```powershell
rg -n "maya/tools/MtoULiveLink|unreal/Plugins/MtoULiveLink" AGENTS.md README.md README_CN.md CHANGELOG.md docs composite unreal/ToolsLab.uproject -g "*.md" -g "*.uproject"
python -m unittest discover -s tests -v
python tools/validate_repository.py --json
python tools/package_maya_tool.py MtoULiveLink --json
python tools/package_unreal_plugin.py MtoULiveLink --engine 5.7 --json
```

Expected: only explicit migration instructions in the approved composite design
and this implementation plan match; all commands pass.

- [x] **Step 5: Commit documentation and ToolsLab discovery**

```powershell
git add AGENTS.md README.md README_CN.md CHANGELOG.md docs composite unreal/ToolsLab.uproject
git commit -m "docs(mtou): document composite installation layout"
```

### Task 4: Production Verification and Clean Handoff

**Files:**
- Verify: `composite/MtoULiveLink/maya/MtoULiveLink/`
- Verify: `composite/MtoULiveLink/unreal/MtoULiveLink/`
- Verify: `unreal/ToolsLab.uproject`

**Interfaces:**
- Consumes: the completed migration and local stock Maya/Unreal installations.
- Produces: verified stock-host acceptance evidence and a clean Git working tree.

- [x] **Step 1: Run Maya 2022.4 tests and Ruff**

```powershell
$maya_root = '<Maya-2022.4-install-root>'
& "$maya_root\bin\mayapy.exe" -m unittest discover -s 'composite\MtoULiveLink\maya\MtoULiveLink\tests' -v
ruff check composite/MtoULiveLink/maya/MtoULiveLink/scripts composite/MtoULiveLink/maya/MtoULiveLink/tests
```

Expected: all 15 Maya tests pass and Ruff reports no errors.

- [x] **Step 2: Run stock UE 5.7.4 build**

```powershell
$ue_root = '<stock-UE-5.7.4-install-root>'
& "$ue_root\Engine\Build\BatchFiles\Build.bat" UnrealEditor Win64 Development "$PWD\unreal\ToolsLab.uproject" -WaitMutex -NoHotReloadFromIDE
```

Expected: UnrealBuildTool discovers the external plugin directory, compiles or validates both MtoULiveLink modules, and exits successfully.

- [x] **Step 3: Run all MtoULiveLink Automation tests**

```powershell
& "$ue_root\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\unreal\ToolsLab.uproject" -unattended -nop4 -nosplash -NullRHI -DDC-ForceMemoryCache -ExecCmds='Automation RunTests MtoULiveLink;Quit' -TestExit='Automation Test Queue Empty'
```

Expected: all 9 Automation tests pass.

- [x] **Step 4: Restore ignored/test-mutated project state and run final checks**

Restore only UE config files changed by the Automation run, confirm generated outputs remain ignored, then run:

```powershell
python -m unittest discover -s tests -v
python tools/validate_repository.py --json
git status --short
git diff --check
```

Expected: tests and validation pass, `git diff --check` is empty, and status contains only intentional migration changes before the final commit.

- [x] **Step 5: Commit the verified migration closeout**

```powershell
git add -A
git commit -m "chore(mtou): complete composite migration acceptance"
```

Do not push and do not remove the retained feature worktree as part of this task.
