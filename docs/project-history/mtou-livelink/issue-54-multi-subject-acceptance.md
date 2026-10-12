# Issue #54: independent prop and second-character preview

Current review: **2026-10-12, changes required; issue remains open**.
Implementation and reviewed baseline: `5add03e155c8ea8eb8b98765c9627cd309696887` (remotely accessible).
R-005, R-007 and R-008 remain open; R-009 records the real-pair anchor defect.
R-001/R-002/R-003/R-004/R-006 remain verified closed.
The [current handoff](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-5883765955) owns the next action.
This record and its compact evidence are published on `codex/mtou-preview-workflow`.
The user authorized publication for comment consolidation on 2026-10-12.

## Scope and inputs

This is the bounded two-subject realtime prototype, independently runnable from
[its README](../../../composite/MtoULiveLink/prototypes/multi-subject/README.md).
[The protocol](../../../composite/MtoULiveLink/prototypes/multi-subject/protocol.md)
contains the identity/time/space proposal, cache-only design and proposed product
contract inventory. Product v9 integration, cache implementation and ownership UI
remain with #47, after #52–54; Additional Parts remain distinct from this pairing.

The user-approved scope from 2026-10-10 is animated C01 plus C02 instead of an
unavailable standalone heroine arm. Paths below are relative to `MtoU_tmp`:

| Subject | Maya input and root | Unreal mesh |
| --- | --- | --- |
| character | `C01_Body_IdleStand02_ChangeClothes.ma`, referencing `SK_C01.ma`; `\|SK_C01:Group\|SK_C01:root` | `/Game/Animation/C01/Rigging/SK_C01_Clothes_09_All` |
| prop (C02) | `ceshi/C02/SK_C02_05MH.ma`; `\|prop:Group\|prop:root` | `/Game/Animation/C02/Rigging/SK_C02_CombineBody_Clothes_05` |

`SK_C01_CombineBody_Clothes_12` requires `upperarm_r_twist_0001`, absent from this
older animated rig. Its refusal remains valid. No additional arm asset is needed.

## Current findings and completion conditions

| ID | Current status | Result and remaining condition |
| --- | --- | --- |
| R-001/R-002 | Verified closed | Session identity and serial-based forward/backward/hold timing pass. |
| R-003/R-004/R-006 | Verified closed | Ownership re-init, undriven-target reference restore and Times/Remove argument parsing pass, including a real Maya peer. |
| R-005 | Open, confirmed defect, P1 | Non-essential bind differences now pass, but the full target skeleton still enters mapping and any optional ambiguity vetoes the pair. Map against necessary dependencies before optional handling. |
| R-008 | Open, confirmed contract deviation, P1 | Declaration-order ambiguity and unique assignment regressions pass. The product's globally duplicated-short-name precondition is still absent; restore it and adapt fixtures to exercise legal rename cases. |
| R-009 | Open, confirmed test/placement defect, P2 | RealAssetPair reinserts the source root's 90-degree import convention through its Anchor after bind projection. Correct world placement and use an independent world-space oracle. |
| R-007 | Open, acceptance gap, P2 | Full real declarations, four frames and lifecycle now pass. Correct-world-space BaseColor and numerical acceptance remain blocked by R-009; strict pairing remains blocked by R-005/R-008. |

### R-005: necessary dependencies must define the mapping target

`MtoUMultiSubjectProtocol.cpp:1248–1287` constructs the complete Skeleton target,
runs mapping on it, then rejects any ambiguity. Required flags only narrow rest
and coverage checks. In the original prop fixture, target `PropTipEnd` (index 3)
is outside all-LOD positive weights and their ancestors. Renaming it `End12` and
declaring `End` and `End1` at the same source parent rejects in both orders.
Removing those optional sources immediately accepts the same necessary bones.
The original fixture also accepts. `NonRequiredAmbiguity` fails two assertions.

Construct the required target graph with original Skeleton indices, preserving
required coverage, hierarchy and ambiguity rejection. Optional mappings must not
alter acceptance of the required graph. Ambiguous optional bones can remain at
reference pose with explicit reporting. Test both orders and optional candidates
that interfere with an otherwise unique required assignment.

### R-008: retain the import-rename precondition

The original two-source counterexample and existing unique-assignment/order tests
pass. However a single globally unique `Joint` now drives required target `Joint12`
(index 3, one rename report). `UniqueNameRename` fails the rejection assertion.
Prototype Protocol:835 omits the product Negotiator's `SourceCount > 1` condition.
This was explicitly retained by review-02 and acknowledged as omitted in dev-03;
an existing permissive prototype test does not authorize changing that contract.

Reuse global short-name counts with parent scope, exact priority, suffix shape and
unique matching. A unique short name must not use this import-rename exception.
Adapt valid rename fixtures to duplicated short names in distinct source branches.
An old counterexample lacking the precondition should still reject, but need not
report ambiguity; test ambiguity with inputs that satisfy the precondition.

### R-009/R-007: the world-space oracle must be independent

At t=120 the original Maya C01 head converted to UE axes is
`(6.843089, 3.659079, 167.400818)` cm. The actual UE component head is
`(6.843093, 3.659080, 167.400817)`: sampling and component projection keep it upright.
The root has a 90-degree import convention. Tests:2917–2919 multiply that sampled
root into `(CommonPlacement * ProbeRootWorld) * WorldLift`, reintroducing the
convention after projection. An independent UE FTransform calculation measures
90.000003 degrees extra rotation and changes the common position from
`(320,-180,440)` to `(320,-40,220)`. Both fresh BaseColor images show the pair lying
sideways. This is a world placement defect, not evidence of bone stretching or a
sideways source scene.

The current bone residual only validates the component pose; root-world residual
validates application of the supplied Anchor. The probe comparison compares two
Maya inputs. None independently compares actual UE world landmarks to converted
Maya world landmarks under the intended common placement. Equal, zero-position
roots mask the issue in the pair-relative checks.

Separate import bind conventions, source scene/ancestor motion and common UE
placement. Apply each once. Add independent world-space landmarks and cases with
nonzero separation, distinct root orientations and moving parents, then capture
properly oriented BaseColor with both objects framed. Camera rotation or raising
the pair is not a substitute for correcting world-space transforms.

## Review execution and evidence

Windows, Maya 2024, UE 5.7.4. This review executed the checks below against
`5add03e155c8ea8eb8b98765c9627cd309696887` without modifying runtime source. A separate diagnostic plugin preserves
all 18 original source files byte-for-byte and adds only the linked review tests.

| Check | Observed result |
| --- | --- |
| Prototype Python discovery | 23/23 passed |
| Maya host tests | 104 checks, zero failures |
| ToolsLab and isolated diagnostic builds | Succeeded |
| ToolsLab prototype suite with real fixture peer | 15 Success entries; 14 executed, RealAssetPair opt-in skipped. Four frames, Remove=2, Times=1,3,2,2. |
| Separate real C01+C02 run, Backups Content shell, non-NullRHI | RealAssetPair Success, with the R-009 oracle gap described above |
| Full declarations | C01 1399 driven / 647 required; C02 1502 driven / 187 required; no source-only or undriven bones |
| Real frames and Morphs | Times 1→120→60→60; maximum component pose delta 0.001510; 428 Morph comparisons, max difference 0.000001236 |
| Real lifecycle | Remove C02 independently; C01 changes 54.763369 cm between real samples; disconnect restores both to reference; wrong-target renegotiation refuses and correct declarations apply |
| Review diagnostics | NonRequiredAmbiguity: 2 failing assertions; AnchorConvention: 1; UniqueNameRename: 1 |
| Original Maya scene probe | t=1/120 world-space head/pelvis/root/neck coordinates, scene not saved |
| Repository and report gates | Repository validator passed; 37 repository tests passed with no skips; review and source archive validated by issue_report.py |

Original Maya scenes, rig references and target mesh assets were not saved.
The existing real-pair digest/dirty checks passed. UE-generated ToolsLab config
changes were removed after the run. Known missing KawaiiPhysics errors from an
old production Anim Blueprint remain expected input limitations in the original
test. Maya 2022, sustained playback, product cache and product integration were
not tested and are not new prototype acceptance requirements.

Maintained review evidence:

- [ReviewDiagnostics.cpp](issue-54-review-evidence/ReviewDiagnostics.cpp): the three
  independent counterexamples. Compile in an isolated copy of the prototype's
  Private/Tests directory, run `Automation RunTests Issue54Review` with NullRHI.
- [results.json](issue-54-review-evidence/results.json): condensed actual diagnostic
  output and baseline provenance; expected failing assertions demonstrate blockers.
- [source_orientation.py](issue-54-review-evidence/source_orientation.py): run with
  Maya mayapy and positional arguments `<C01 animation scene> <output.json>`;
  opens without saving and samples the original source independently.
- [source-orientation.json](issue-54-review-evidence/source-orientation.json): this
  review's source-world coordinates, free of private absolute paths.

Complete local output remains in `../.tmp/mtou-issue54/review-20261012/`:
`ue-suite/report`, `realpair/report`, `realpair/c01-c02` (fresh BaseColor and lifecycle),
`diagnostics/report`, `rename-precondition/report`, host logs and the isolated host.
It is retained for unresolved-finding reproduction, not claimed remotely available.
Earlier `fix-20261010/` and `review-20261010/` remain comparison evidence; the shared
Backups shell in `fix-20261008/backups-copy/` remains a test host. No archival
closeout or scratch cleanup of those ongoing inputs is claimed.

## Handoff and preservation

Implement the remaining R-005/R-008 contract corrections and R-009 placement fix,
then re-run R-007 with full C01/C02 inputs while keeping lifecycle and Sequencer
regressions. Keep issue #54 open. Do not integrate product changes or add cache
implementation under this review.

[The comment archive](issue-54-comment-archive.json) preserves all seven reports
captured before consolidation, with exact IDs, bodies and timestamps. Its immutable
publication is commit `3d602e7ac0a7582e23176068644016ee354cc131`; the remotely read
archive was checked against every captured report before any deletion. Earlier
snapshots remain in Git history; an older entry's machine path was already
redacted before this review.

Four comments remain: one current summary, unchanged review-03, and two short
archive pointers at the original review-02 and dev-03 IDs. These pointers preserve
the latest review's source links. Superseded dev-01, review-01 and dev-02 were
removed after preservation and summary read-back. There were no separate human
discussion comments. The latest review's body and update timestamp are unchanged,
and no live source link points to a removed comment. Issue #54 remains open;
this consolidation changed neither findings nor acceptance scope. Publication
includes the review evidence and source archive; raw host evidence stays local
for unresolved-finding reproduction. This was comment-only consolidation, not
completed-issue archival closeout.
