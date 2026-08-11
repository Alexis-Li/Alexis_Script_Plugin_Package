# Maya Project Guide

Use this guide to choose a project shape. The normative Maya naming,
compatibility, safety, layout, and test rules live in
[`maya/AGENTS.md`](../maya/AGENTS.md).

## Choose the Smallest Project Shape

| Need | Project shape |
| --- | --- |
| One directly runnable Script Editor file | `maya/scripts/<ToolName>/` |
| Plug-in registration, multiple functional files, resources, persistent UI, tests, installation, or reusable API | `maya/tools/<ToolName>/` |
| One product spanning Maya and another host | `composite/<ProjectName>/maya/<ToolName>/` |

Start with the first shape that satisfies the requirement. A direct script
exposes `run()` or `main()` and does not depend on the repository being on
`PYTHONPATH`. Structured tools put only the runtime folders they use directly
under the project root.

A Maya component of a cross-host product instead uses
`composite/<ProjectName>/maya/<ToolName>/` and follows the same runtime layout,
version, packaging, and safety rules. It must remain installable without its
sibling host source tree. Keep Maya-specific tests beside this runtime, but
keep README, changelog, license, and project rules at the composite root.

```text
<ToolName>/
├─ scripts/       # optional
├─ plug-ins/      # optional
├─ icons/         # optional
└─ presets/       # optional
```

## Verification path

1. Run pure-Python tests outside Maya when the tested code permits it.
2. Run integration checks with a supported Maya or `mayapy`.
3. Run `python tools/validate_repository.py` from the repository root.
4. Confirm the project documentation states only versions actually tested.
