# Issue tracker: GitHub

Issues and specs for this repository live in GitHub Issues. Run `gh` commands
from the repository clone so the remote is inferred automatically.

## Operations

- Create: `gh issue create --title "..." --body "..."`
- Read: `gh issue view <number> --comments`
- List: `gh issue list --state open --json number,title,body,labels,comments`
- Comment: `gh issue comment <number> --body "..."`
- Add or remove labels: `gh issue edit <number> --add-label "..."` or
  `--remove-label "..."`
- Close: `gh issue close <number> --comment "..."`

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

Claiming a ticket with `gh issue edit <number> --add-assignee @me` is the first
write operation. Resolve it by commenting with the result, closing the issue,
and adding the durable context reference to the map.
