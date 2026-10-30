# Issue #54: independent prop and second-character preview

Current acceptance: **2026-10-13, review-04 passes the bounded prototype; issue closed, no open acceptance blockers**.
Latest implementation and reviewed baseline: `b593dad42849705873c4d1b59f633992a966f4e3` (published on `codex/mtou-preview-workflow`).
R-001 through R-009 are verified closed. The [current handoff](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-5883765955)
records the issue state; product integration remains with #47.

## Scope and inputs

The [independently runnable prototype](../../../composite/MtoULiveLink/prototypes/multi-subject/README.md)
demonstrates two realtime subjects. Its [protocol](../../../composite/MtoULiveLink/prototypes/multi-subject/protocol.md)
delivers identity, common time, space, exclusive preview ownership, cache-only
design, the product-contract inventory and a minimum implementation order.
Product v9 integration, a multi-object cache and product UI are outside this issue.
The 2026-10-10 agreed real-input scope uses animated C01 plus C02 instead of an
unavailable standalone heroine arm. Synthetic full-body/arms pairing remains tested.

Paths in this table are relative to the supplied `MtoU_tmp` asset directory.

| Subject | Maya input and root | Unreal target |
| --- | --- | --- |
| character | `C01_Body_IdleStand02_ChangeClothes.ma`, referencing `SK_C01.ma`; `\|SK_C01:Group\|SK_C01:root` | `/Game/Animation/C01/Rigging/SK_C01_Clothes_09_All` |
| prop (C02) | `ceshi/C02/SK_C02_05MH.ma`; `\|prop:Group\|prop:root` | `/Game/Animation/C02/Rigging/SK_C02_CombineBody_Clothes_05` |

The real run references the complete existing C02 rig through a fixture manifest;
it does not prune either declaration. C01 declares 1399 bones (647 necessary),
C02 declares 1502 (187 necessary). The C02 target is a body/clothes mesh without
a head; that visible boundary is not a streaming loss. `SK_C01_CombineBody_Clothes_12`
still legitimately refuses this older animated rig because `upperarm_r_twist_0001`
is absent. The acceptance does not extend compatibility to that different mesh.

## Acceptance conclusions

The rows follow the issue's existing criteria without adding or rewriting them.

| Criterion | Result and evidence boundary |
| --- | --- |
| Explicit root/target pairs, strict skeleton matching | Accepted. Necessary target bones and ancestors decide coverage, hierarchy, ambiguity and bind; optional mapping cannot veto them. Duplicate short names may use the scoped product rename rules; globally unique short names may not. No Additional Parts or automatic retargeting. |
| Evaluated reference animation, common frames, name isolation and cleanup | Accepted. Maya host checks, real TCP peers and C01/C02 stream cover common frame/time, same-name bone/Morph isolation, independent removal, disconnect and scene/world cleanup. |
| Root motion, reference hierarchy, Actor/socket/Sequencer placement | Accepted within the bounded world-anchor profile. Independent world landmarks and authored offset/orientation/parent-motion tests prove one placement application. Moving socket-parent targets are explicitly refused to prevent double movement; arbitrary socket workflows are not implemented. |
| Full-body/arms target matching and renegotiation | Accepted. A real Maya synthetic arms peer rejects the full-body declaration, negotiates the correct rig and streams four frames; C01/C02 separately exercises a real different-skeleton rejection and renegotiation. |
| Explicit Sequencer takeover and restore | Accepted in the minimum transient Sequence. The suite evaluates the existing skeletal animation before, during and after preview, restores per-target writers and animation state, and checks no sequence asset edits. Arbitrary production sequences and KawaiiPhysics behavior are not claimed. |
| Realtime mandatory, cache design only | Accepted. Both live-pair paths run; protocol design specifies common range/fps, atomic readiness, incompatibility and failure behavior. No cache implementation is required or delivered. |
| Runnable prototype, sample, state/error evidence, contract inventory | Accepted. Component instructions, the protocol and the evidence below are present. Product integration remains a joint #47 decision after #52–54, not a unilateral prototype cutover. |

## Finding disposition

| ID | Current result |
| --- | --- |
| R-005 | Verified closed. `NecessaryScopeMapping` uses duplicated short-name donors so optional rename ambiguity is real, accepts both orders, preserves necessary assignment beside an optional candidate, and still refuses necessary ambiguity. `NonEssentialBindDifferences` and the original independent `NonRequiredAmbiguity` diagnostic pass. |
| R-008 | Verified closed. Original `UniqueNameRename` diagnostic rejects unique `Joint → Joint12`; `RenameCandidateContracts` covers valid duplicates, competing candidates, unique allocation and exact-name priority. Complete C01/C02 still negotiates. |
| R-009 | Verified closed. Both real targets use the same placement `(320,-180,40)`, yaw 25 degrees, without adding sampled root rotation or a world lift. Actual UE world positions match independently sampled Maya world positions transformed by that anchor. The authored fixture also covers offset roots, different root orientations and parent movement. |
| R-007 | Verified closed within the agreed C01/C02 scope. Complete declarations, four frames, Morph values, actual world placement, upright BaseColor presentation, independent removal, reference restoration and fresh negotiation pass. |
| R-001/R-002/R-003/R-004/R-006 | Remain verified closed by session/time, command-line, ownership/re-init, restoration and cleanup regressions. |

The old `AnchorConvention` diagnostic hard-codes the removed placement expression;
its original counterexample remains in immutable Git history, not as a current implementation test.
Review-04 executes the current world-placement test and real Maya/UE world oracle.
The original optional-ambiguity diagnostic alone would now also pass because its
unique names cannot rename; the duplicated-name `NecessaryScopeMapping` assertions
are therefore essential evidence for R-005, not just the legacy diagnostic result.

## Independently executed verification

Windows 11, Maya 2024, UE 5.7.4. Runtime source was unchanged throughout this review.
The real-asset host is a disposable project shell whose Content junction points
to the supplied Backups Content. The diagnostic host's 18 original source files
match the reviewed plugin byte for byte; its extra `ReviewDiagnostics.cpp` matches
the preserved review-03 artifact by SHA-256.

| Check | Observed result |
| --- | --- |
| Prototype Python discovery | 23/23 passed |
| `mayapy tests/maya_host_multi_subject_tests.py --result <scratch>/host.json` | 104 checks, zero failures |
| UE `Build.bat UnrealEditor Win64 Development <project> -WaitMutex -NoHotReloadFromIDE -NoUBA`, both hosts | Succeeded, targets up to date |
| `Automation RunTests MtoUMultiSubjectPrototype`, NullRHI, character-prop, Frames=4, Remove=2, Times=1,3,2,2 | 17 Success entries; **16 executed**, RealAssetPair skipped without asset flags and run separately below |
| `Automation RunTests MtoUMultiSubjectPrototype.RealMayaPeer`, character-arms, Frames=4, Times=1,3,2,2 | Passed. Expected first `skeleton_mismatch`; correct negotiation, four frames and disconnect restoration pass |
| `Automation RunTests Issue54Review.NonRequiredAmbiguity+Issue54Review.UniqueNameRename` | 2/2 passed |
| `Automation RunTests MtoUMultiSubjectPrototype.RealAssetPair`, non-NullRHI, inputs above, Frames=4, Times=1,120,60,60 | Success, zero assertion errors, **174 asset-load warnings** |
| Real pair measurements | Bone residual 0.001510; root/anchor residual 0.000523; 428 Morph values, max difference 0.000001236 |
| Independent real world oracle | 9 Maya samples collected at t=1/120/60; **5 actual UE comparisons** at t=120 (C01 head/pelvis, C02 pelvis) and t=1 (C01 head/pelvis), max 0.000006 cm |
| Authored world placement | Landmark translation ≤0.000005 cm, rotation ≤0.000002 degrees, pair separation error 0.000005 cm |
| Real lifecycle | Independent C02 removal, C01 continuation, reference restoration and renegotiation pass; pose-change metric 54.763369 |
| Repository gates | `python tools/validate_repository.py` passed; `python -m unittest discover -s tests -v`: 37/37, no skips; final diff/report/archive validation passed |

Bone/root/lifecycle residuals are `max(translation-component cm, rotation radians,
dimensionless scale)` in `MtoUSubjectTransformDelta`; they are **not** pure-centimetre
metrics or vertex-deformation measurements. World-landmark errors above are actual
Euclidean distances in centimetres. The warning total consists of 171 empty engine
versions, one absent custom version and two absent KawaiiPhysics script packages;
100 matching KawaiiPhysics graph messages are additionally expected/suppressed by
the existing test. This is not acceptance of the production animation blueprint.
Maya checks confirm the input scene and referenced rig files remain unchanged;
the real test confirms target packages are not dirtied.

## Evidence and reproduction

The current history tree retains one acceptance record, one source-comment archive
and four evidence files, each with a specific purpose:

| Retained file | Purpose |
| --- | --- |
| [Source-comment archive](issue-54-comment-archive.json) | Ten comment snapshots: the six live before this closeout plus four earlier source comments already removed from the thread. Original IDs, authors, bodies, timestamps and previous versions preserve provenance. |
| [Review-04 results](issue-54-review-evidence/review-04-results.json) | Individual test states, all 104 Maya host check outcomes, peer exit/ownership states, measured observations, warning categories, raw-artifact hashes and metric limits. Closeout extracts existing evidence; it does not rerun host acceptance. |
| [Common-anchor BaseColor](issue-54-review-evidence/review-04-basecolor-c01-c02.png) | Upright but overlapping source placement; materials were still checker placeholders, so it is not a material-quality reference. |
| [Separated BaseColor](issue-54-review-evidence/review-04-basecolor-c01-c02-separated.png) | Both target meshes upright and completely framed with material colors. The prop anchor moves only for visibility; numeric spatial acceptance precedes this display offset. |
| [C02 fixture template](issue-54-review-evidence/real-c02-fixture.template.json) | Recreates the exact full-rig reference input without preserving a disposable host or machine-specific paths. Replace both placeholders, save as `fixture.json` in the chosen manifest directory, and pass that directory as `MtoUMultiSubjectPropRigDir`. |

The four superseded review-03 files (`ReviewDiagnostics.cpp`, `results.json`,
`source_orientation.py`, `source-orientation.json`) are recoverable from
[published immutable history](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/tree/def59ac959d956422fd680a85b34e90417691679/docs/project-history/mtou-livelink/issue-54-review-evidence).
Their remote contents were compared against the local originals before removal
from the current tree. Maintained prototype tests now cover those findings;
the prior failed expression is not an active acceptance gate.

Use the component READMEs for build and peer flags. The real run supplies
`MtoUMultiSubjectMayaScene`, `MtoUMultiSubjectCharacterRoot`, the two mesh flags,
`MtoUMultiSubjectPropRigDir` using the template above, and
`MtoUMultiSubjectRealScenario=c01-c02`, with Frames=4 and Times=1,120,60,60.
Run against a disposable host that can read the supplied Backups Content and
load the prototype plugin; keep all report/log/fixture output in scratch.
Neither the original Maya assets nor the Backups project is an archival output.

Maya 2022, sustained-playback performance, arbitrary saved production sequences,
product cache and product integration were not tested by review-04 and are not
additional prototype closure requirements.

## Archival state

The source archive at `02235a6604e7ad4cb9d903897abba9130aab43ce` is published on
the current branch and was retrieved through GitHub's blob API. Its content
matched the committed Git blob byte-for-byte (SHA-256
`8a013318d549721e16fbc703b36c8dfffe4172bee9214c9040d2c7ccdf18b8b8`). It keeps
ten comment snapshots: six were live before this closeout, and four older source
comments had already been removed. Each of the six live snapshots matched the
remote ID, complete body, author and creation/update timestamps before the
current summary was updated.

The current summary (comment ID `5883765955`) cites the archive and original
review-03, dev-04 and review-04 IDs. The five superseded live reports/archive
pointers (`6095144149`, `6096911178`, `6116403868`, `6117167023`, `6136464547`)
were re-read immediately before deletion, matched their archived bodies and
timestamps, and were removed by exact ID. Final pagination found exactly one
live comment: the current summary marker. The issue remains `CLOSED`; one current
agent summary remains, with no superseded live reports or separate human
discussion comments.

The completed inventory covered both scratch locations and known host outputs.
Removed issue-owned temporary material:

| Removed path | Purpose |
| --- | --- |
| `../.tmp/mtou-issue54/` | About 11 GB of disposable hosts, fixtures, test outputs and logs from all inventoried rounds. The two `Content` junction entries were removed individually after verifying both resolved to the production Backups project's Content directory; the target was not traversed or deleted. |
| `../.tmp/mtou-issue54-review/` | Empty issue-owned directory. |
| `../.tmp/issue-54-body.json`, `../.tmp/issue-54-comments.txt` | Superseded issue snapshots. |
| `unreal/Saved/MtoUMultiSubjectTests/` | One ignored prototype test result JSON. |
| `../.tmp/mtou-issue54-closeout-20261013/` | This closeout's inventory, helper scripts, snapshots, drafts and API payloads; removed after source evidence was committed and the final API read-back completed. |

The final scan found no retained #54 scratch files or output directories. The
repository-local `.tmp/` remains absent. Shared `MtoU_tmp/C01`,
Backups project, other issues' scratch, host-wide caches, the current branch and checkout were preserved. The active
Maya 2020 UI process had no #54 scratch path in its command line and was left
untouched; no Maya 2024/mayapy or Unreal host process for this issue was active.
