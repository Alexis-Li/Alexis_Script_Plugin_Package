# Independent project repositories

The collection can contain both directly owned projects and independent Git
submodules. Each submodule occupies one complete project root at a registered
standalone-host or composite path. Its internal source layout belongs to that
repository, so future host components can remain in the same product repository.

## Get the sources

Clone the collection with its pinned projects:

```powershell
git clone --recurse-submodules https://github.com/Alexis-Li/Alexis_Script_Plugin_Package.git
```

In an existing checkout, after checking for local submodule changes:

```powershell
git submodule update --init --recursive
```

A plain clone can run collection checks without initializing independent
projects; validation reports their missing checkouts as warnings. Initialize a
project before developing or installing it. Do not run an update over active
child work without preserving that work first.

## Ownership

| Concern | Owner |
| --- | --- |
| Project code, documentation, Issues, PRs and releases | Independent project repository |
| Host behavior, unit tests, packaging and version tools | Independent project repository |
| Mount path, clone URL, pinned commit and project index | Collection repository |
| Shared collection templates and registration checks | Collection repository |

Run Git and gh inside the owning repository. The child AGENTS.md must work
without parent files and describe its host contracts. Collection packaging and
version commands do not manage independent projects. Collection CI checks both
plain and recursive checkouts; each child runs its own product CI.

## Work on a project

Initialization commonly checks out a pinned commit with detached HEAD. Inspect
`git status` and switch to the intended development branch inside the child
before editing. Commit and push product changes there, then review them in that
repository. A submodule commit does not automatically change the collection pin.

To adopt a published and reviewed commit, check it out in the child after
preserving local work, then stage that project's mount path in the collection.
Review the old and new commit IDs and commit the pin change on a collection
branch. The validator compares the checkout HEAD to the staged pin, so stage an
intentional update before running collection gates.

Do not use automatic tracking of a moving branch tip for collection releases.
Push child commits before the collection commit that references them; a fresh
recursive clone must be able to fetch every pin.

## Register or migrate a project

1. Work in an isolated collection branch/worktree, leaving active sibling
   development untouched. Preserve unpublished files and decisions first.
2. Prepare an independent repository with its own rules, metadata, docs, tests
   and CI. Preserve relevant history or document its immutable source baseline.
   Do history extraction in a separate clone.
3. Transfer outstanding Issues to the product tracker. Re-read transferred
   bodies and repair relative issue references, paths and dependencies.
4. Publish and verify the initial child commit. Add a submodule at a complete
   project root with `git submodule add <HTTPS-clone-URL> <project-path>`.
   The collection tracks .gitmodules and a gitlink, not the child's source files.
5. Move proposal content to the product's README pair, CONTEXT.md, ADRs and
   Issues as appropriate. Verify the destinations before deleting the proposal.
   Update collection indexes and links.
6. Run repository gates, product checks and a fresh recursive clone. Check the
   sibling tree and original worktree against their starting state. Deliver the
   collection change through a separate PR; integrate it at a suitable boundary
   for active development branches.

The registry accepts credential-free HTTPS clone URLs and complete project
paths. It rejects missing or duplicate registrations, missing gitlinks, paths
outside registered project locations, and unregistered nested repositories.
An initialized checkout must match its pinned commit. Project source validation
belongs to the child and must not depend on collection validators.
