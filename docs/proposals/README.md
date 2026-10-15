# Proposals

A proposal is the requirement baseline of a product that is still in design and
has no installable component yet. It lets requirements, research conclusions,
and proof-of-concept plans be reviewed in the repository before the product's
category (standalone host project or composite product) and layout are decided.

## Rules

- Use one lowercase kebab-case directory per product:
  `docs/proposals/<project-name>/`. Its main document is `requirements.md`.
- The umbrella GitHub issue links to the proposal as its requirement baseline.
  Task status, decisions in progress, and review stay in issues and PRs; update
  the proposal only when an accepted decision changes the baseline.
- Keep the proposal standalone: a reader should understand purpose, scope,
  constraints, and acceptance without earlier chat or drafts. Do not keep
  superseded versions side by side; Git history retains them.
- Do not commit third-party code, unlicensed reference material, or archives
  into a proposal. Record conclusions and links instead.
- A proposal is not a user manual and is not covered by host verification.

## Leaving the proposal stage

When development creates the owning project, move the surviving content to its
owners: user-facing behavior to the project README pair, vocabulary to
`CONTEXT.md`, durable decisions to `docs/adr/`, and remaining requirements to
issues. Then delete the proposal directory and update links to it. Abandoned
proposals are deleted after their umbrella issue records the outcome.

## Current proposals

- [SPtoU Texture Sync](sptou-texture-sync/requirements.md): Substance 3D
  Painter texture changes update existing Unreal Engine Texture assets.
