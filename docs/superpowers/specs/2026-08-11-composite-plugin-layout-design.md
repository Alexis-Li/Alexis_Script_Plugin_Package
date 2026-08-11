# Composite Plugin Layout Design

Date: 2026-08-11
Status: Approved for implementation

## Summary

The repository gains a top-level `composite/` category for products that ship
cooperating components for more than one host application. Each product stays
in one PascalCase project directory, and each host component remains a complete
copyable unit below its lowercase host directory.

MtoU_LiveLink becomes the first composite product. Its Maya and Unreal source
move together without changing runtime identifiers, protocol, version, or
supported host versions.

## Target Layout

```text
composite/
├─ AGENTS.md
└─ MtoULiveLink/
   ├─ README.md
   ├─ README_CN.md
   ├─ CHANGELOG.md
   ├─ LICENSE
   ├─ maya/
   │  └─ MtoULiveLink/
   │     ├─ scripts/
   │     ├─ tests/
   │     ├─ README.md
   │     ├─ README_CN.md
   │     ├─ CHANGELOG.md
   │     └─ LICENSE
   └─ unreal/
      └─ MtoULiveLink/
         ├─ MtoULiveLink.uplugin
         ├─ Source/
         ├─ README.md
         ├─ README_CN.md
         ├─ CHANGELOG.md
         ├─ LICENSE
         └─ AGENTS.md
```

`composite` is a repository category and therefore uses lowercase kebab-case.
`MtoULiveLink` remains PascalCase because it is the public project and component
identifier. The lowercase `maya` and `unreal` directories identify hosts; the
nested PascalCase directory is the independently installable component root.

## Installation Contract

Users copy components, not the entire composite project:

- Maya source: copy the contents required from
  `composite/MtoULiveLink/maya/MtoULiveLink/` into Maya's matching runtime
  directories, or execute `scripts/MtoULiveLink.py` directly.
- Unreal source: copy
  `composite/MtoULiveLink/unreal/MtoULiveLink/` to
  `<Project>/Plugins/MtoULiveLink/`. The destination must contain
  `<Project>/Plugins/MtoULiveLink/MtoULiveLink.uplugin`.

The composite-level bilingual READMEs explain the combined workflow and point
to the component READMEs for host-specific installation. Component archives
remain independent; the migration does not introduce a combined binary or
source archive.

## Repository Tooling

Existing CLI contracts remain stable:

```powershell
python tools/package_maya_tool.py MtoULiveLink --json
python tools/package_unreal_plugin.py MtoULiveLink --engine 5.7 --json
```

Each packager resolves a named component from its legacy single-host location
or its composite location. Exactly one matching location is allowed; finding
both is an actionable ambiguity error. Other Maya tools and Unreal plugins keep
their current paths and behavior.

The repository validator recognizes `composite/` as a project category and
checks:

- a composite project uses a PascalCase directory name;
- it has paired root READMEs, changelog, and license;
- it contains at least two host component directories;
- each Maya component satisfies the existing Maya tool contract;
- each Unreal component contains exactly one root descriptor and paired
  READMEs;
- generated outputs and nested repositories remain forbidden.

No new production dependency or project template is added. A composite project
template is deferred until a second composite product demonstrates a stable
shared shape.

## ToolsLab Discovery

`unreal/ToolsLab.uproject` keeps `MtoULiveLink` enabled and adds this relative
external plugin search directory:

```json
"AdditionalPluginDirectories": [
  "../composite/MtoULiveLink/unreal"
]
```

That directory contains the installable `MtoULiveLink/` plugin folder. The
plugin remains independently buildable and does not depend on the Maya sibling
or any file outside its own root.

## Documentation and Governance

The root bilingual READMEs list MtoU_LiveLink once as a composite product rather
than as two unrelated tools. `AGENTS.md`, the workspace specification, naming
rules, development guidance, release guidance, MtoU design, implementation
plan, and production acceptance record use the new paths.

`composite/AGENTS.md` defines the cross-host boundary and repeats the minimum
Maya and Unreal component rules required when editing below this new root.
Historical Git commits retain the old paths; current documentation does not use
them except when explicitly describing the pre-migration layout.

## Migration and Compatibility

The move uses Git renames so component history remains traceable. These paths
must be absent after migration:

```text
maya/tools/MtoULiveLink/
unreal/Plugins/MtoULiveLink/
```

The following remain unchanged:

- Maya entry file and `__version__`;
- Unreal descriptor name, modules, and `FriendlyName`;
- TCP protocol, endpoint, subject, and production behavior;
- stock Unreal Editor 5.7.4 and Maya 2022.4 support claims;
- the explicit absence of a compatibility claim for the modified UE build.

## Verification

Migration is complete only when all of the following pass from the repository
root:

1. Repository unit tests, including composite discovery and ambiguity cases.
2. Repository structural validation with no version-control-visible generated
   path.
3. Maya tests under Maya 2022.4 `mayapy` from the composite path.
4. Ruff checks for the moved Maya source and tests.
5. Both existing packaging dry-runs with unchanged versions and non-zero file
   counts.
6. Stock UE 5.7.4 build of `unreal/ToolsLab.uproject`, proving external plugin
   discovery and both module paths.
7. All `MtoULiveLink` Unreal Automation tests.
8. A final search showing no unintended current references to either old
   component path.

Generated build and test outputs remain ignored and are not committed.
