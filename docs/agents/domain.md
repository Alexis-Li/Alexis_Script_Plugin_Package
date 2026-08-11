# Domain docs

This repository uses a multi-context domain-documentation layout.

## Before exploring

If `CONTEXT-MAP.md` exists at the repository root, read it and then read each
referenced `CONTEXT.md` relevant to the task.

Read applicable decision and history records from:

- `docs/adr/` for active repository-wide decisions
- `<project>/docs/adr/` for active project-specific decisions
- `docs/project-history/<project>/` for stable architecture, migration, and
  acceptance records from completed work

If these files do not yet exist, proceed silently. Domain-modeling workflows
create context files and active ADRs when terminology or decisions become
durable.

## Layout

```text
/
|-- CONTEXT-MAP.md
|-- docs/adr/                            # Active repository-wide decisions
|-- docs/project-history/<project>/      # Completed durable records
|-- maya/scripts/<ToolName>/
|   |-- CONTEXT.md
|   `-- docs/adr/
|-- maya/tools/<ToolName>/
|   |-- CONTEXT.md
|   `-- docs/adr/
|-- unreal/Plugins/<PluginName>/
|   |-- CONTEXT.md
|   `-- docs/adr/
`-- composite/<ProjectName>/
    |-- CONTEXT.md
    `-- docs/adr/
```

`CONTEXT-MAP.md` indexes contexts and their relationships. Create it with the
first real context rather than as an empty placeholder. Create context files
and ADR directories lazily; their presence is not required before work begins.

When work is completed, integrate its durable architecture, migration, and
acceptance conclusions into `docs/project-history/<project>/` according to the
repository documentation rules.

## Vocabulary

Use terms as defined by the relevant `CONTEXT.md`. Avoid synonyms that its
glossary explicitly rejects.

If a required concept is absent, reconsider whether the term belongs to the
project or record the gap for domain modeling.

## ADR conflicts

Surface any proposal that conflicts with an existing ADR or stable project
history. Name the record and explain why reopening the decision may be
justified rather than silently overriding it.
