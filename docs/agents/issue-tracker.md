# Issue and pull request workflow

Use `gh` from this clone so GitHub resolves the repository from its remote.
The issue owns the problem, scope, acceptance criteria and product decisions.
The PR owns the implementation diff, review findings and check results.

## From requirement to integration

1. **Define the task.** Read the issue and all its comments. Specify observable
   acceptance criteria, exclusions and dependencies. Use stable `AC-01` IDs
   where useful; cite exact wording for older issues. Separate suggested
   implementation from agreed requirements. Prototype acceptance stays within
   its agreed prototype scope.
2. **Choose the delivery boundary.** Small wording, typo, link and formatting
   fixes, and small edits to rules, workflows, templates or `CHANGELOG.md`, go
   directly on the current working branch when they change no code, tests,
   tools, CI, packaging, version or product decision. They need no separate
   branch, PR, development report or review report. They reach `main` with
   their branch; the current branch is `main` only when no development branch
   is in progress. Run applicable checks and use normal commits without
   rewriting shared history. Use a PR if size or impact is uncertain.
   All other work, including development tasks, code changes and significant
   changes, goes through a PR. Branch from the intended integration branch
   with `feature/<topic>` for new features, tools or documentation, or
   `fix/<topic>` for corrections; use a short kebab-case topic. Inspect local
   changes before switching branches. Serial work can reuse a checkout;
   concurrent writers need isolated checkouts and clear file ownership.
3. **Deliver a reviewable change.** Record full starting and ending SHAs,
   target branch, resulting behavior, checks actually run and remaining gaps
   in the PR description, and link it from the issue.
4. **Review the actual change.** Read requirements and the complete diff at
   the named commits, inspect affected callers, and check acceptance,
   compatibility, failure paths and unrelated changes. Verify delivery claims
   against code and evidence. Publish the review on the PR with its full base
   and head SHAs, scope, conclusion, blockers and evidence.
5. **Integrate the reviewed result.** Every merge requires a published review
   of the latest PR head SHA. The user's merge authorization does not replace
   that review. Recheck the head and target branch, assess changes since review
   and run affected checks. Use `gh pr merge --match-head-commit <reviewed-head>`.
   GitHub's **Automatically delete head branches** setting stays enabled.
   It removes only the remote branch: after the merge, run
   `git fetch --prune origin`, then remove the head branch's local worktree
   and delete the local head branch when the worktree is clean and the branch
   is contained in the target branch. Leave a dirty or unmerged checkout in
   place and report it; do not remove another active session's worktree.
   Record the resulting `main` or integration-branch SHA in the issue's progress
   comment, together with the PR, covered ACs and pending acceptance. Do not
   add a separate post-merge SHA comment to the PR.
6. **Complete the issue.** Close when agreed acceptance is covered, blockers
   are resolved and the delivery is on its intended integration branch.
   Research/prototype tasks close against their own deliverable. Closure
   includes the comment consolidation below: the status comment says `已关闭`
   and contains the final review, integration SHA and acceptance results. Do
   not add a separate closing comment. Milestone integration into `main` and
   [release](../release-process.md) are separate steps.

Commit, push, PR creation, merge, tracker updates, closure and comment deletion
follow the task's authorization. Report the achieved state precisely:
implemented, reviewed, integrated, host-verified or released.

### Review baselines and integration checks

- Approval covers the stated scope and full commit SHA, including the comparison
  base. Uncommitted work needs a reproducible diff with relevant untracked inputs;
  it is not a reviewed commit.
- Later changes affecting that scope need incremental review and relevant tests.
  Inspect changed paths and dependencies before reusing evidence. Target-branch
  changes and conflict resolutions can alter behavior even with an unchanged
  task head. Explain reuse where reviewed code and its assumptions remain valid.
- Inspect target drift and the integration result separately from the head SHA
  guard. Squash/rebase changes commit identity; retain the reviewed range and
  resulting integration SHA in the issue's progress or status comment.
- Distinguish executed checks, inspected historical results and source-based
  inference. Current GitHub CI runs repository validation, repository unit
  tests and the system-Python tests of FlattenMeshToUV and NitroPoly on pushes
  and PRs. Other owning-project tests and host-application
  acceptance (Maya, Unreal, Substance 3D Painter, ...) need their own evidence.
- A scoped review may pass while whole-issue acceptance remains pending.
  Self-tests alone do not establish acceptance. Findings need trigger, impact,
  evidence and completion condition; severity and blocking are separate judgments.
- Allocate `R-001` style finding IDs across the issue after reading existing
  findings. Keep each ID through fixes and re-review. A claimed fix remains
  **已修复，待复验** until reviewed. Classify open work as **已确认缺陷**,
  **待验证风险**, **验收缺口** or **后续候选**; candidates become blockers only
  through agreed scope changes. Include the completion condition and next actor.

GitHub does not allow a PR author to approve their own PR. If publication uses
that account, record the review as a comment with baseline, scope, conclusion
and evidence; it is not a GitHub approval event.
See [GitHub's review rules](https://docs.github.com/en/pull-requests/how-tos/review-pull-requests/approving-a-pull-request-with-required-reviews).

Use `Refs #<issue>` in PR descriptions and close explicitly after acceptance
and integration. Closing keywords on a PR targeting the default branch can
close the issue before its acceptance and comment consolidation are complete.
See [GitHub's linking rules](https://docs.github.com/en/issues/tracking-your-work-with-issues/using-issues/linking-a-pull-request-to-an-issue).

## Current decisions and information owners

Write documents, issues and comments as current facts, without change notes or
labels for removed content. Requirements and decisions can become outdated.
Agree a changed decision with the user, then rewrite it in place in its issue
body, ADR, document or comment, replacing the old wording. Keep decisions in
force until a replacement is agreed.

| Surface | Content |
| --- | --- |
| Issue body | Requirements, acceptance criteria, dependencies and agreed scope |
| PR description/review | Change range, delivery, code findings and checks |
| Ordinary issue comments | New progress, review and acceptance facts; integration result |
| Status comment on tidying or closure | Consolidated current state, useful evidence and next actions |
| Project documentation | Current usage/contracts; version changes in the changelog |
| Project history | Selected decisions and essential evidence under the [retention rules](../project-history/README.md) |

Progress, review and acceptance each use ordinary comments as work proceeds.
Edit incorrect or outdated content in one's own comment in place; a new comment
carries new facts only. Readers start from the newest comment, so every new
comment ends with a footer of one to three lines, `**当前状态**：…`, giving the
state of the whole issue (not only that comment's topic) and the next step with
who takes it. Between tidies the last comment is therefore the current state;
earlier comments and an earlier status comment are not edited just to keep them
current. An open issue accumulates comments until the user asks to tidy it or
it closes.

## Status comments

Tidying on the user's request or closing an issue produces one status comment.
Use `tools/issue_report.py template` and `check` for that comment; its status
and next-step sections serve as its footer. Ordinary progress, review and
acceptance comments use the content appropriate to them plus the `当前状态`
footer.
PR descriptions use the repository template.

```markdown
<!-- issue-status:v1 issue=65 -->
## 状态

- 状态：进行中 / 待审核 / 待验证 / 受阻 / 可关闭 / 已关闭
- 基线：完整实现、最终审核和集成 SHA 或包版本，说明尚未完成的阶段
- 更新日期：YYYY-MM-DD

### 简介
### 相关提交
### 已完成
### 未完成
### 开发边界
### 决定
### 宿主验收
### 下一步
```

- **简介**: the issue's purpose and current conclusion.
- **相关提交**: PRs and commits with full SHAs and a concise purpose; final
  review link. The baseline distinguishes implementation, review and integration.
- **已完成 / 未完成**: results by AC or requirement ID, evidence links, unresolved
  findings, acceptance gaps and completion conditions. `可关闭` or `已关闭` needs
  all agreed acceptance covered and no blocking findings.
- **开发边界**: inclusions, exclusions, items moved elsewhere and evidence limits.
- **决定**: decisions in force, linking their maintained ADR or requirement.
- **宿主验收**: tested SHA/package, exact host builds and relevant environment,
  neutral sample identity, reproduction steps, measurements with units, result
  per AC and untested scenarios. Distinguish checks run now from historical
  evidence and explain why reused evidence applies. Use `无` when inapplicable.
- **下一步**: concrete next actions and responsible actor, or `无` when complete.

Keep the first-line marker and section order; fill every section, using `无`
when empty. Use the task's timezone for the date. The checker validates the
structure, status, date and portable paths; the publisher verifies evidence,
link availability, unresolved blockers and semantic consistency.

Issue/PR bodies, reviews, comments and archive metadata use repository-relative
paths or placeholders such as `<Maya2024>`, `<Project.uproject>` or `<scratch>`.
Do not publish local drive paths, user directories or secrets. Existing
`docs/project-history/` comment archives are historical files: their original
comment bodies remain exact, while metadata and current writing obey this
portable-path policy. Repository validation checks those artifacts.

## Publishing safely

Generate drafts in the scratch location resolved under `AGENTS.md`:

```powershell
python tools/issue_report.py template --issue 65 --output "$scratch/issue-65-status.md"
python tools/issue_report.py check "$scratch/issue-65-status.md" --json
```

Replace placeholders before publishing. Templates refuse to overwrite files.
Exit codes: 0 success, 1 invalid content/file error, 2 CLI usage error.

Read the issue and paginate all comments before writing. For a status comment,
match its marker and issue number; reconcile multiple matches before editing by
exact numeric comment ID. Never use `--edit-last` on a shared account.

Create with `gh issue comment <number> --body-file <draft.md>`. Before editing,
read `gh api repos/{owner}/{repo}/issues/comments/<id>` and verify `issue_url`,
ownership, body, marker when present and `updated_at`. Re-read immediately before
writing and reconcile concurrent changes. Send the full body through a UTF-8
JSON file with `gh api --method PATCH repos/{owner}/{repo}/issues/comments/<id>
--input <payload.json>`. Read back the saved body and return the URL. After
ambiguous timeouts, read before retrying. Serialize writes per issue; one
coordinator publishes delegated results. Read-before-write is not an atomic lock.

## Closure and requested archival

Consolidate comments only when the user asks to tidy them or when closing the
issue. These actions include the following unless explicitly narrowed:

1. **Inventory.** Read the issue, all comments, linked evidence, both scratch
   locations and known task outputs. Establish ownership and ongoing use;
   preserve shared inputs and uncertain files.
2. **Keep useful knowledge.** Move content needed for a future decision or
   regression check into its maintained repository owner (ADR, README, project
   history or other relevant document); requirements remain in their owning
   issue. Extend the existing owner and link it from the status comment. Drop
   outdated material. Commit and publish necessary repository content within
   authorization before deleting its source comments. If publication is
   unavailable, retain dependent comments and report the unfinished work.
3. **Consolidate.** Create or edit the validated status comment with the current
   conclusion, useful evidence and next actions. On closure it says `已关闭`
   and includes the final review, integration SHA and acceptance results.
   Re-read the saved comment and verify its links. Replace references to comments
   being removed with their maintained destination.
4. **Clean.** Re-read targeted comments, reconcile concurrent edits and delete
   the other comments by exact ID. Remove task-owned scratch once needed evidence
   has reached its owner, including helpers, drafts, payloads and results.
   Validate resolved paths before deletion and preserve shared or uncertain files.
5. **Verify.** Paginate comments to confirm that one status comment remains;
   check evidence links, publication state and scratch after the last helper run.
   Report any incomplete cleanup with its reason and next action. Close without
   a separate closing comment when acceptance and integration are complete.

Existing historical records and comment archive JSON files remain available for
published references. Issue closure and a product release are separate actions.

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

Local checks and repository instructions guide publication. Server enforcement
depends on GitHub settings; current CI does not validate acceptance claims or
authorize merges.
