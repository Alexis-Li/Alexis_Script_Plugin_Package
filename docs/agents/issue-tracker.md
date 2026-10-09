# Issue tracker: GitHub

Issues and specs for this repository live in GitHub Issues. Run `gh` commands
from the repository clone so the remote is inferred automatically.

## Report protocol (v1)

This is the repository-wide contract for agent-authored issue delivery, review,
re-verification, and consolidation, including work produced by `to-spec`,
`code-review`, or other skills. It does not impose a template on human discussion,
ordinary chat updates, or exploratory questions. Skills still own their research
or review method; this protocol owns the published result. In particular, keep
Standards and Spec findings distinguishable inside a review report.

The issue body owns requirements, scope, acceptance criteria, and dependencies.
Use stable `AC-01` acceptance IDs where practical; for an older issue, cite the
exact criterion until IDs can be assigned without rewriting its meaning. A
formal report must not silently add acceptance requirements or turn a suggestion
into a product decision. Prototype completion is judged against the prototype's
scope, not against completion of the eventual product.

Use three report types:

| Kind | Purpose | Allowed status |
| --- | --- | --- |
| `development` | Deliver implementation or a fix for review | 待审核 / 部分完成 / 受阻 |
| `review` | State the result for a specific delivery and review scope | 通过 / 需修改 / 证据不足 |
| `summary` | Maintain one current, actionable handoff for the whole issue | 进行中 / 待审核 / 修复中 / 待验证 / 受阻 / 可关闭 / 已关闭 |

Generate the required structure with `tools/issue_report.py template`. The
Chinese field names and headings are fixed for these reports; explanatory prose
may use the issue's language. Keep the generated first-line marker: it identifies
the protocol, kind, issue, and stable report key. Use keys such as `dev-01`,
`review-01`, and `review-02`; the one summary always uses `current`. The key
identifies a report, not its author or the most recently posted comment.

Every report has these non-empty header fields:

| Field | Required meaning |
| --- | --- |
| 状态 | One status from the kind's list above |
| 基线 | Full implementation/review commit SHA and whether it is local or remotely accessible. A summary distinguishes latest implementation from latest reviewed baseline. Uncommitted work must be identified as such and accompanied by a reproducible diff/artifact, including relevant untracked inputs; do not label it a reviewed commit. |
| 范围 | The acceptance items or findings addressed. State partial approval explicitly. A summary states what remains in this issue and why it remains open or can close. |
| 依据 | Requirement and source delivery/review links, or an accessible archive entry identifying the original report. A date alone is not a source. |
| 阻塞项 | Open blocker IDs in this report's scope, comma-separated (`R-001, R-002`), or `无`. Summary blockers cover the whole issue. |
| 本次验证 | 本轮执行 / 历史复核 / 混合 / 未执行. A consolidation that only reorganizes comments uses 未执行; describe any inspection of old reports separately. |
| 更新日期 | Actual date in the task's timezone, `YYYY-MM-DD` |

### Development report

After `## 开发交付` and the header, use these sections in order:

1. `### 交付结果`: resulting behavior and delivery entry points, mapped to AC or
   finding IDs. A claimed fix is **已修复，待复验** until reviewed.
2. `### 验证与证据边界`: commands/operations, environment, observed results and
   accessible evidence. Distinguish this run from reused results.
3. `### 剩余事项`: unfinished implementation, failed/skipped checks, limitations
   and blockers; write `无` when empty.
4. `### 下一步`: the concrete review or implementation action needed.

Self-tests do not grant whole-issue acceptance. Report what changed and why it
matters; avoid a chronology of debugging attempts or repeated full logs.

### Review report

After `## 审核结论` and the header, use these sections in order:

1. `### 结论`: approval/changes/evidence gap for the stated scope, and whether it
   supports whole-issue acceptance. A static review can pass while host acceptance
   is still pending; do not present that as whole-issue approval.
2. `### 审核问题`: stable finding IDs, category, severity, whether each blocks the
   stated acceptance, and current status. For each finding give trigger, actual
   behavior, impact, supporting criterion/code/reproduction, and completion or
   re-verification condition. Write `无` when there are no findings.
3. `### 验证与证据边界`: checks actually performed, old evidence inspected and why
   it still applies, and uncovered areas. Preserve Standards/Spec categories and
   distinguish confirmed behavior from source-based inference.
4. `### 后续处理`: fixes, evidence needed, or the conditions for closing.

Allocate `R-001` style IDs across the issue; retain them through fixes and
re-verification. Re-read the current handoff before allocating IDs. Track findings
as 未解决, 已修复待复验, 已复验关闭, or 不采纳（with evidence/authorized decision）.
Severity and acceptance blocking are separate decisions. New findings get new IDs;
do not recycle IDs or use a change in round number to erase an unresolved finding.

### Current handoff / consolidation

After `## 当前结论` and the header, use these sections in order:

1. `### 未闭环事项`: current work first. Use a table with ID, type, problem or
   gap, completion criterion, blocking status, and owning issue/next actor.
2. `### 已验证成果与边界`: organize by acceptance items, preserving useful
   capabilities and their limits. Separate approved contracts from proposals.
3. `### 验证依据`: map acceptance items to results, source report/artifact,
   tested baseline/environment and whether the result was executed or only
   inspected in the original review. State what the consolidation itself did.
4. `### 接手入口`: delivery location, essential run conditions, reproduction
   entry points and next actions. Link to maintained instructions rather than
   duplicating whole parameter tables and command manuals.
5. `### 记录索引`: implementation commits, acceptance records and source archives.

Classify outstanding work as **已确认缺陷**, **待验证风险**, **验收缺口**, or
**后续候选**. A candidate belongs to its parent/future specification and does not
automatically block this issue. Do not mark an unobserved risk as reproduced, a
fixture as production acceptance, or parameter agreement as image equivalence.
Explain misleading aggregates such as expected negative tests retained in an
`errors[]` array, skipped host checks counted in a passing suite, and metrics
whose units cannot support a broader performance or quality claim.

Keep the summary useful for both a quick status decision and the next developer.
There is no hard length cap. Retain details necessary to understand a blocker or
reproduce it; move exhaustive mappings/logs into existing appropriate records.
Do not require a new history document for each small task.

### Updating and consolidating reports

- Read the issue body, current handoff and relevant later reports before work.
  The current handoff is a derived view; stronger evidence for the relevant
  baseline takes precedence over an older conclusion. Conflicts that cannot be
  resolved remain explicit gaps, not silently averaged conclusions.
- Publish a report at a meaningful delivery/review boundary. Correct small
  mistakes or add evidence for the same scope to the same report. A new delivery,
  review baseline, or materially different conclusion gets a new report key.
- Maintain at most one current handoff, updating it after formal reports. An
  initial delivery need not create a second identical comment: create the handoff
  at the first review/rework boundary or consolidation, then keep it current.
  An issue-body link may point to it without replacing the requirements.
- Consolidate when a review/fix cycle completes, current conclusions become
  difficult to locate, or the user requests it. Merge by AC/finding ID, not by
  concatenating rounds. Preserve unresolved dissent, user decisions and still
  applicable evidence. Never turn development assertions into reviewer approval.
- Preserve original reports before substantive replacement. Corrections to
  factual errors may be made in place, with source history recoverable. During
  active development, retain separate delivery/review reports as needed. A user
  request to consolidate comments or close out a completed issue authorizes
  removing superseded agent-authored reports after durable archival and
  verification: keep exactly one final summary among agent reports, including
  the final review's conclusion, provenance and limits in that summary. Merely
  marking old reports superseded or hiding them does not complete this request.
  Preserve human discussion, user decisions and unresolved dissent; do not infer
  permission to delete these or unrelated comments. Without a consolidation or
  closeout request, maintain the current handoff without deleting comments.
- For archives use `github-issue-comment-archive/1`: `issue`, `archived_on`,
  `purpose`, `current_status_comment`, and `comments` containing each original
  `id`, `created_at`, `updated_at`, `author`, `original_url`, and full `body`.
  Preserve timestamps and bodies rather than rewriting history to the new format.
  Store authorized archives in the project's existing history directory; inspect
  for secrets/private paths before publishing. A local untracked file or scratch
  directory is not a completed durable handoff. If remote publication is not
  authorized, commit the archive locally and explicitly report remote availability
  as pending; do not claim an inaccessible GitHub link works.
- Consolidation is complete only when sources are recoverable, IDs/links and
  baselines are correct, outstanding work has completion criteria and ownership,
  and referenced documents that claim current status agree. Correct contradicted
  current conclusions in place; preserve historical facts in the archive. If
  synchronization is unavailable, list it as unfinished work.
- For completed issues, the current history tree keeps the final acceptance
  record, one source-comment archive and only evidence needed to substantiate
  acceptance, a material limit, or an otherwise unreproducible finding. Keep
  reproduction inputs only when maintained tests do not replace them. Do not
  copy every run, failed iteration, draft or disposal log out of scratch into
  history. Preserve already-committed intermediate material through an immutable
  Git reference; extract unique durable facts before removing redundant files.
  Give each retained evidence file a purpose in the existing record or a compact
  evidence index. Update existing records instead of adding a closeout diary.
- Before deleting comments, commit and publish the source archive, verify the
  remotely accessible content against the captured IDs, bodies and timestamps,
  and publish/read back the validated final summary. Replace live references to
  removed reports with archive references identifying the original report ID;
  historical archived bodies stay intact. Re-read each targeted comment just
  before deletion and reconcile concurrent changes first. Delete by exact ID,
  then paginate all comments to verify the final count and sole summary marker.
  If publication or deletion is unavailable, report closeout as incomplete.
- Report actual remaining comment counts (agent reports versus preserved human
  discussion), the retained evidence set and reasons, reference checks, and
  commit/publication state. A unique current marker, successful archive check,
  clean scratch directory or passing tests alone does not establish closeout.
- Product integration decisions stay in their owning parent issue. Consolidating
  a child does not authorize integration or raise its agreed acceptance bar.

### Publishing and verification

Generate drafts in the scratch directory required by `AGENTS.md` (resolve it
from the Git root first). These commands use an already-resolved `$scratch`:

```powershell
python tools/issue_report.py template development --issue 53 --key dev-03 --output "$scratch/issue-53-dev-03.md"
python tools/issue_report.py check "$scratch/issue-53-dev-03.md" --json
python tools/issue_report.py check-archive docs/project-history/mtou-livelink/issue-53-comment-archive.json --json
```

Replace every placeholder, then run `check` before writing to GitHub. Templates
are incomplete by design; output files are created exclusively, never overwritten.
Exit codes: 0 success, 1 invalid report/archive or file error, 2 CLI usage error.
The tool has no network writes or new dependencies. It validates structure,
status vocabulary, declared blockers, dates and source-archive fields; it cannot
prove evidence truth, link availability, completeness or semantic consistency.
The publishing agent remains responsible for those checks.

For a new formal comment, read **all** issue comments (paginate) and look for the
same kind/issue/key marker before creating it. If a matching report already
exists, compare it and use its exact numeric comment ID. Never use `--edit-last`:
agents may share one GitHub account. Multiple matching markers are a conflict to
resolve, not permission to overwrite an arbitrary comment. Legacy reports can be
migrated during their next authorized update; first preserve their sources.

Use `gh issue comment <number> --body-file <draft.md>` for creation. For editing,
read `gh api repos/{owner}/{repo}/issues/comments/<id>` and verify its `issue_url`,
author/ownership, marker, body and `updated_at`. Immediately before writing,
re-read and compare the body/timestamp to the version being edited. On a change,
reconcile first. Send the full body through a UTF-8 JSON file and
`gh api --method PATCH repos/{owner}/{repo}/issues/comments/<id> --input <payload.json>`.
Read back and compare the saved body and marker after either operation; report
the actual comment URL. After an ambiguous timeout, read before retrying to avoid
duplicate creation. File contents are data, never shell-interpolated commands.

Serialize publication per issue: one coordinating agent writes and other agents
return findings to it. Read-before-write is not an atomic concurrency lock; do
not use concurrent writers on the same handoff. Existing authorization persists;
these checks do not add an extra approval step or authorize otherwise unrequested
external writes. A passed report does not itself authorize closing an issue.

Close only when the issue's agreed acceptance is covered, blockers are resolved,
the applicable review/acceptance supports closure, and task authorization permits
it. Publish the final result and update the handoff before closing; avoid an extra
duplicate closing comment. Deferred product work belongs in its owning issue.

Repository instructions and the mandatory local check are the current enforcement
mechanism. They do not prevent a client with GitHub write credentials from
bypassing the workflow. Other agent clients must load this same contract through
their own instruction entry point; do not duplicate it into every skill. Server
monitoring or a credential-controlled publisher is a separate deployment choice,
not something this repository currently claims to enforce.

## Operations

- Create: `gh issue create --title "..." --body "..."`
- Read: `gh issue view <number> --comments`
- List: `gh issue list --state open --json number,title,body,labels,comments`
- Comment: validate the report, then `gh issue comment <number> --body-file <draft.md>`
- Add or remove labels: `gh issue edit <number> --add-label "..."` or
  `--remove-label "..."`
- Close: `gh issue close <number>` after the acceptance and authorization checks above

For multiline content, use `--body-file` with a temporary Markdown file
appropriate to the active shell. Run write operations only when the task
authorizes external changes.

## Pull requests as a triage surface

**PRs as a request surface: no.**

If this is changed to `yes`, external pull requests enter the same triage
workflow as issues. Read them with `gh pr view <number> --comments` and
`gh pr diff <number>`.

GitHub shares numbering between issues and pull requests. If a bare reference
such as `#42` is ambiguous, try `gh pr view 42` and then `gh issue view 42`.

## Skill terminology

When a skill says "publish to the issue tracker," create a GitHub issue.

When a skill says "fetch the relevant ticket," run
`gh issue view <number> --comments`.

## Wayfinding

A wayfinding map is one issue labelled `wayfinder:map`. Its child tickets use
one of these labels:

- `wayfinder:research`
- `wayfinder:prototype`
- `wayfinder:grilling`
- `wayfinder:task`

Use GitHub sub-issues where available. Otherwise, maintain a task list in the
map and begin each child issue with `Part of #<map>`.

Represent blockers with GitHub issue dependencies. If dependencies are
unavailable, begin the blocked issue with `Blocked by: #<number>`.

When authorized to work through the tracker, claim a ticket with
`gh issue edit <number> --add-assignee @me` before other tracker writes. Resolve
it with the formal result and current handoff, add the durable context reference
to the map, and close only when the conditions above are met.
