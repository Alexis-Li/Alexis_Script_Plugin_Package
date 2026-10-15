# Project history

This directory holds knowledge that remains useful after a task is complete:
important design tradeoffs, migration constraints and acceptance evidence that
cannot be recovered adequately from maintained code, tests or tracker records.

## Choose the owner before writing

| Information | Where it belongs |
| --- | --- |
| Requirements, scope decisions, unfinished work and task status | Issue |
| Requirement baseline and research for a product still in design | [Proposal](../proposals/README.md) until the project exists |
| Delivery, reviewed diff, findings and routine test results | PR, or issue reports for direct-branch work |
| Current installation, usage, recovery and supported limits | Project README pair or maintained user guide |
| Version-by-version behavior changes | Project changelog |
| Current terminology and architectural decisions | Existing context/decision documents under [domain guidance](../agents/domain.md) |
| Completed migration/design rationale, calibration or essential host evidence | Existing topic record here; create one only if no suitable owner exists |
| Code evolution and recoverable intermediate documents | Git history |
| Raw runs, generated fixtures, report drafts and one-off helpers | Scratch while needed; remove after required evidence is preserved |

Routine fixes and successful repeat test runs normally finish in the tracker.
Before adding a history record, identify the future decision or verification
that needs it, check whether existing code/tests/docs already answer it, and
extend the existing topic where possible. Organize by capability or milestone;
several related issues can share a record.

## What a durable record contains

Keep the conclusion, accepted scope and material limits first. Include the
baseline, environment/input identity, evidence source and reproduction entry
needed to judge that conclusion. Preserve distinctions between implementation,
review, integration and host acceptance.

Retain raw evidence when it supports a result that would otherwise be lost:
production-host measurements, calibration inputs, owner visual acceptance,
hard-to-reproduce failures or an independent oracle absent from maintained
tests. Identify each retained file's purpose in its owning record. Large raw
outputs belong in an appropriate published artifact with an immutable reference;
capture the relevant result and provenance here.

Successful routine logs, repeated reports, debugging rounds, publication
receipts and cleanup diaries add no durable record by themselves. Consolidate
their useful facts into the existing owner. A closed issue alone does not require
an acceptance document, JSON evidence bundle or comment archive.

## Index and maintenance

- Use one lowercase kebab-case directory per project. Its `README.md` is a
  topic index with only the milestones needed to orient the reader.
- Use purpose-based names for new records, such as `camera-acceptance.md` or
  `cache-migration.md`; identify source issues inside. Keep existing filenames
  when renaming would break published links.
- Link to the authoritative explanation. Indexes do not repeat test counts,
  fixes, acceptance tables or the changelog.
- Keep historical conclusions tied to their baseline. For a later acceptance,
  update the relevant topic without presenting old evidence as a new run.
  Current contracts belong in maintained product documentation.
- Before removing redundant committed records, extract unique conclusions,
  update references and verify an accessible immutable Git version. Git does
  not retain remote issue comments or untracked host outputs.
- Source-comment archives are needed when preserving comments before substantive
  replacement/deletion. They retain exact provenance and are not current task
  status. Existing archives stay available for published references.

Follow the [tracker protocol](../agents/issue-tracker.md#closure-and-requested-archival)
for requested comment consolidation and completed-issue closeout, including
source preservation, publication verification and issue-owned scratch cleanup.

## Projects

- [MtoU Live Link](mtou-livelink/README.md)
