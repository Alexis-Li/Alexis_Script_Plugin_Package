# Issue and pull request workflow

Use `gh` from this clone so GitHub resolves the repository from its remote.
The issue owns the problem, scope, acceptance criteria and product decisions.
The delivery owns the implementation diff, review findings and check results.

## From requirement to integration

1. **Define the task.** Read the issue body, its current handoff when present,
   and relevant later reports. Specify observable acceptance criteria, exclusions
   and dependencies. Use stable `AC-01` IDs where useful; cite exact wording for
   older issues. Separate suggested implementation from agreed requirements.
   Prototype acceptance stays within its agreed prototype scope.
2. **Choose the delivery boundary.** For a feature, substantial fix or work
   overlapping other tasks, branch from the intended integration branch and
   open a PR back to it. Use `codex/<short-task-name>`. A small, isolated change
   can use the current branch with an explicit commit range. Serial work can
   reuse a checkout; concurrent writers need isolated checkouts and clear file
   ownership. Inspect local changes before switching branches.
3. **Deliver a reviewable change.** Record full starting and ending SHAs, target
   branch, resulting behavior, tests actually run and remaining gaps. Use the
   PR description and link it from the issue. For direct-branch work, publish
   a development report on the issue.
4. **Review the actual change.** Read requirements and the complete diff at the
   named commits, inspect affected callers, and check acceptance, compatibility,
   failure paths and unrelated changes. Verify delivery claims against code and
   evidence. Review on the PR, or publish an issue review report for a
   direct-branch delivery.
5. **Integrate the reviewed result.** Recheck the delivery HEAD and target
   branch, assess changes since review and run affected checks. Record the
   integration commit and any pending host acceptance. For direct-branch work,
   check the same conditions on the branch already containing the delivery.
6. **Complete the issue.** Close when agreed acceptance is covered, blockers
   are resolved and code delivery is present on its intended integration branch.
   Research/prototype tasks close against their own deliverable. Link the final
   review and integration result. Milestone integration into `main` and
   [release](../release-process.md) are separate steps.

Commit, push, PR creation, merge, tracker updates and closure follow the task's
authorization. Report the achieved state precisely: implemented, reviewed,
integrated, host-verified or released.

### Review baselines and integration checks

- Approval covers the stated scope and full commit SHA, including the comparison
  base. Uncommitted work needs a reproducible diff with relevant untracked inputs;
  it is not a reviewed commit.
- Later changes affecting that scope need incremental review and relevant tests.
  Inspect changed paths and dependencies before reusing evidence. Target-branch
  changes and conflict resolutions can alter behavior even with an unchanged
  task HEAD. Explain reuse where reviewed code and its assumptions remain valid.
- With `gh pr merge`, pass `--match-head-commit <reviewed-head>` to guard against
  PR-head changes. Inspect target drift and the integration result separately.
  Squash/rebase changes commit identity; retain the reviewed range and resulting
  integration SHA.
- Distinguish executed checks, inspected historical results and source-based
  inference. Current GitHub CI runs repository validation and repository unit
  tests on pushes and PRs. Owning-project tests and host-application acceptance
  (Maya, Unreal, Substance 3D Painter, ...) need their own evidence.
- A scoped review may pass while whole-issue acceptance remains pending.
  Self-tests alone do not establish acceptance. Findings need trigger, impact,
  evidence and completion condition; severity and blocking are separate judgments.

GitHub does not allow a PR author to approve their own PR. If publication uses
the author's account, record the review as a comment with baseline, scope,
conclusion and evidence; it is not a GitHub approval event.
See [GitHub's review rules](https://docs.github.com/en/pull-requests/how-tos/review-pull-requests/approving-a-pull-request-with-required-reviews).

For PRs targeting a development branch, use `Refs #<issue>` and explicitly
close after acceptance and integration. PR-description closing keywords apply
when targeting the default branch, which is `main` here.
See [GitHub's linking rules](https://docs.github.com/en/issues/tracking-your-work-with-issues/using-issues/linking-a-pull-request-to-an-issue).

## Where to put the result

| Surface | Content |
| --- | --- |
| Issue body | Requirements, acceptance criteria, dependencies and approved scope changes |
| PR description/review | Change range, delivery, code findings, checks and integration result |
| Issue without a PR | Development and review reports for the named commit range |
| Current handoff, when needed | Unresolved work and links to delivery/review evidence |
| Project documentation | Current usage/contracts; version changes in the changelog |
| Project history | Selected decisions and essential evidence under the [retention rules](../project-history/README.md) |

One delivery followed by one conclusive review needs no duplicate summary.
Create one `current` handoff when multiple deliveries, review/rework rounds,
partial acceptance or conflicting reports make the next action hard to find.
Once present, update it at meaningful boundaries. An issue-body link can point
to it while keeping requirements in the body.

With a PR, an issue update can be status, PR/review link and remaining acceptance
gap. Keep detailed findings at their source. A closing update links the final
result and integration SHA. Progress, questions and link updates use ordinary
comments.

## Formal issue reports (v1)

Use `tools/issue_report.py template` and `check` for agent-authored development,
review and consolidated reports published on issues. PR descriptions use the
repository template; PR reviews retain baseline, scope, conclusion, blockers
and evidence. Publish at meaningful handoffs, not after every commit or test.

| Kind | Purpose | Allowed status |
| --- | --- | --- |
| `development` | Deliver implementation/fixes for review | 待审核 / 部分完成 / 受阻 |
| `review` | Review a specified delivery and scope | 通过 / 需修改 / 证据不足 |
| `summary` | Current whole-issue handoff | 进行中 / 待审核 / 修复中 / 待验证 / 受阻 / 可关闭 / 已关闭 |

Keep the generated first-line marker and fixed Chinese headings. Keys such as
`dev-01` and `review-01` identify reports; the summary key is `current`. A new
delivery, review baseline or materially different conclusion gets a new key.
Correct minor errors or add evidence for the same scope in place.

Every report has these non-empty header fields:

| Field | Meaning |
| --- | --- |
| 状态 | A status for this report kind |
| 基线 | Full implementation/review SHA, comparison base and local/remote availability. Summaries distinguish latest implementation, latest reviewed baseline and integration state. |
| 范围 | Covered acceptance/finding IDs and partial results. Summaries explain what remains or why the issue can close. |
| 依据 | Requirements and source delivery/review links, or accessible archive entries identifying original reports |
| 阻塞项 | Open blocker IDs in scope, comma-separated (`R-001, R-002`), or `无`; summaries cover the whole issue |
| 本次验证 | 本轮执行 / 历史复核 / 混合 / 未执行; comment-only consolidation uses 未执行 |
| 更新日期 | Actual date in the task's timezone, `YYYY-MM-DD` |

Use the following titles and sections in order. Each section may be concise;
write `无` for an empty findings/remaining-work section.

| Kind / title | Sections and content |
| --- | --- |
| development / `## 开发交付` | `### 交付结果`: behavior/entry points mapped to AC/finding IDs; `### 验证与证据边界`: checks, environment, results and provenance; `### 剩余事项`: unfinished work, skipped/failed checks and limits; `### 下一步`: concrete next action |
| review / `## 审核结论` | `### 结论`: scoped result and whether it supports closure; `### 审核问题`: findings and closure conditions; `### 验证与证据边界`: executed checks, historical evidence/applicability, inference and gaps; `### 后续处理`: fixes or closure conditions |
| summary / `## 当前结论` | `### 未闭环事项`: ID, type, gap, completion condition, blocking and next actor; `### 已验证成果与边界`: accepted capabilities/limits; `### 验证依据`: AC-to-source/result/baseline/environment mapping and what this consolidation did; `### 接手入口`: delivery, essential conditions and next action; `### 记录索引`: commits, PRs/reviews and retained evidence |

Allocate `R-001` style IDs across the issue after reading existing findings.
Carry them through fixes as 未解决, 已修复待复验, 已复验关闭 or
不采纳（with evidence/authorized decision）. A delivery's claimed fix remains
**已修复，待复验** until reviewed. Retain IDs when linking PR findings into a
handoff. Include category, severity, blocking, trigger, impact, criterion/code
or reproduction, and completion condition; keep Standards and Spec distinct.

Classify open work as **已确认缺陷**, **待验证风险**, **验收缺口** or
**后续候选**. Candidates do not become blockers without agreed scope changes.
Preserve unresolved dissent and user decisions. Explain metric units, expected
failures, skipped checks and fixture limits. Summaries are derived views:
reconcile stronger evidence for the relevant baseline instead of retaining
stale conclusions. Link maintained instructions rather than copying them.

## Publishing safely

Generate drafts in the scratch location resolved under `AGENTS.md`:

```powershell
python tools/issue_report.py template development --issue 53 --key dev-03 --output "$scratch/issue-53-dev-03.md"
python tools/issue_report.py check "$scratch/issue-53-dev-03.md" --json
```

Replace placeholders before publishing. Templates refuse to overwrite files.
Exit codes: 0 success, 1 invalid content/file error, 2 CLI usage error. The tool
checks structure/declarations; the publisher checks evidence truth, link
availability and semantic consistency.

Read all comments with pagination before creating a report. Match kind/issue/key
and edit an existing match by exact numeric comment ID. Multiple matches need
reconciliation. Never use `--edit-last` on a shared account.

Create with `gh issue comment <number> --body-file <draft.md>`. Before editing,
read `gh api repos/{owner}/{repo}/issues/comments/<id>` and verify `issue_url`,
ownership, marker, body and `updated_at`. Re-read immediately before writing;
reconcile concurrent changes. Send the full body through a UTF-8 JSON file with
`gh api --method PATCH repos/{owner}/{repo}/issues/comments/<id> --input <payload.json>`.
Read back the saved body/marker and return the URL. After ambiguous timeouts,
read before retrying. Serialize writes per issue; a coordinator publishes
delegated results. Read-before-write is not an atomic lock.

## Closure and requested archival

Ordinary closure leaves delivery/review evidence at its source; update derived
handoffs from those records. Select durable knowledge using the
[history retention rules](../project-history/README.md). Git does not preserve
remote comments: capture source reports before substantive replacement, and
verify recoverability of any comment before deleting it.

A request to consolidate comments or to archive/tidy/close out a completed
issue (归档、整理、收尾) includes the following unless explicitly narrowed.
Acceptance/closure and archival completion remain separate states.

1. **Inventory.** Read the issue, reports, referenced history/evidence, both
   scratch locations and known issue outputs. Establish ownership and ongoing
   use; preserve shared inputs and uncertain files.
2. **Preserve.** Consolidate unique decisions and essential evidence into their
   existing owner. For source comments to be replaced/deleted, preserve exact
   IDs, authors, timestamps, URLs and bodies in one archive. Keep human
   discussion, user decisions and unresolved dissent intact.
3. **Publish and verify.** Before deleting comments, commit/publish their source
   archive within authorization, compare remote content against captured sources,
   then publish/read back the validated final summary. Replace live references
   to removed comments with archive references identifying original IDs.
   If publication is unavailable or unauthorized, retain sources and report
   dependent cleanup as pending.
4. **Consolidate and clean.** Re-read targeted comments and reconcile edits.
   Delete superseded agent reports by exact ID, retaining the final summary
   and preserved human discussion/decisions. Remove redundant local records
   after extracting unique content and verifying immutable references. Clean
   obsolete issue-owned scratch, including this closeout's helpers, snapshots,
   drafts, API payloads and results. Validate resolved paths before deletion
   and preserve files needed by other work.
5. **Verify completion.** Paginate comments to confirm final report counts;
   check history links and re-scan scratch after the last helper run. Report
   retained evidence/purpose, actual comment counts, publication state and any
   retained temporary path with reason and next action. Incomplete parts stay
   explicit; a passing archive check alone is not closeout.

Source archives use `github-issue-comment-archive/1`: `issue`, `archived_on`,
`purpose`, `current_status_comment`, and `comments` containing original `id`,
`created_at`, `updated_at`, `author`, `original_url` and full `body`. Preserve
legacy wording. Use the existing project history directory, inspect for secrets
and private paths, then validate:

```powershell
python tools/issue_report.py check-archive <archive.json> --json
```

Archives preserve provenance. Their existence does not require another
acceptance document. Integration decisions belong to the owning parent issue;
consolidation preserves the accepted scope.

## Tracker navigation

- Read issues with `gh issue view <number> --comments`; inspect PRs with
  `gh pr view <number> --comments` and `gh pr diff <number>`.
- List with `gh issue list --state open`; use [triage labels](triage-labels.md).
- When a skill says "publish to the issue tracker", create a GitHub issue.
  Use `--body-file` for multiline issue/PR bodies and comments.
- Issues and PRs share numbering. Resolve an ambiguous reference with
  `gh pr view <number>`, then `gh issue view <number>` if needed.
- A wayfinding map uses `wayfinder:map`; children use `wayfinder:research`,
  `wayfinder:prototype`, `wayfinder:grilling` or `wayfinder:task`. Use
  sub-issues/dependencies when available; otherwise use a task list with
  `Part of #…` and `Blocked by: #…` in child bodies. Link final results from the map.
- When authorized to work through the tracker, claim with
  `gh issue edit <number> --add-assignee @me` before other tracker writes.

Local report checks and repository instructions guide publication. Server
enforcement depends on GitHub settings; current CI does not validate acceptance
claims or authorize merges.
