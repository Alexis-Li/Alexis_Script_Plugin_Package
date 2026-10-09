# Project Development History

This directory preserves stable development history for completed projects.
It records durable architecture, important implementation changes, migrations,
and production acceptance evidence without keeping temporary agent plans.

## Archive Rules

- Use one lowercase kebab-case directory per project.
- Keep a project `README.md` as its timeline and document index.
- Rename durable records to stable purpose-based names such as
  `architecture.md`, `migration.md`, or `stock-engine-acceptance.md`.
- Delete completed implementation plans after their durable decisions and
  evidence have been integrated here or into current project documentation.
- Keep normative repository and platform rules in the applicable `AGENTS.md`;
  history documents may link to those rules but must not duplicate them.
- Git history remains the source for discarded intermediate drafts and detailed
  step-by-step implementation activity.
- Completed issues retain final acceptance conclusions, one source-comment
  archive and the minimum evidence needed to support acceptance or material
  limits. Keep essential reproduction inputs when maintained tests do not
  replace them; explain retained files in the acceptance record or evidence index.
  Do not preserve every development run, failed iteration, publication draft or
  scratch disposal log in the current tree. Extract unique durable facts and
  use immutable Git references for previously committed intermediate material.
- Follow [the issue tracker protocol](../agents/issue-tracker.md) for comment
  consolidation and completed-issue archival. Archival includes consolidating
  the current history tree and cleaning issue-owned scratch, including the
  archival task's own scripts and drafts, after preserving necessary evidence.
  Verify comments, history references and temporary-file remnants before
  reporting completion; publishing an archive alone is not completed closeout.

## Projects

- [MtoU_LiveLink](mtou-livelink/README.md)
