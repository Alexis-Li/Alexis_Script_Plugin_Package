# Issue #54: independent prop and second-character preview

Current delivery: **2026-10-10, R-005/R-008 fixed and R-007 real pair verified;
pending review**. Implementation baseline: the commit that contains this record
(see the delivery comment for its SHA; local then pushed to
`origin/codex/mtou-preview-workflow`). Reviewed baseline before this delivery:
`76ae463400c4a6b493fda5ceda05f0e6bca7b136`.
R-001/R-002/R-003/R-004/R-006 keep their closed findings; R-005, R-007 and
R-008 are **已修复，待复验** within the prototype scope.
The [formal review](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-6095144149)
owns the findings and completion conditions; the
[current handoff](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-5883765955)
is the actionable issue summary.

## Scope and selected assets

This is an editor-only, bounded two-subject realtime prototype. It does not
change MtoULiveLink v9, the single-Subject product, cache playback or the
existing animation export/import workflow. The runnable entry points are in
[the prototype README](../../../composite/MtoULiveLink/prototypes/multi-subject/README.md).
[The protocol](../../../composite/MtoULiveLink/prototypes/multi-subject/protocol.md)
owns the identity/time/space design, cache-only design and proposed product
contract changes. Product integration remains #47's combined decision after
#52–54; completion of a product registry, cache or ownership UI is not added
to this prototype's acceptance scope.

On 2026-10-10 the user explicitly selected **C01 plus C02**, with C02 serving
as the second character in place of an unavailable standalone heroine arm, and
accepted the located C01 animation scene. Missing arm assets are not an
acceptance blocker and the generated Box023 rig does not substitute for the
requested second character. The user also supplied the binding-file directory
`MtoU_tmp/ceshi`, whose `C01/111_MH_Backups.0002.ma` and `C02/SK_C02_05MH.ma`
are byte-identical to the earlier `MtoU_tmp` copies (SHA-256 prefixes
`0c65ae6f…`, `d17c119f…`).

Inputs are relative to the user-supplied `MtoU_tmp` asset directory:

| Role | Maya input / root | UE target |
| --- | --- | --- |
| C01 animated character | `C01_Body_IdleStand02_ChangeClothes.ma`, referencing `SK_C01.ma`; root `\|SK_C01:Group\|SK_C01:root` | `/Game/Animation/C01/Rigging/SK_C01_Clothes_09_All` |
| C02 independent character | `ceshi/C02/SK_C02_05MH.ma`, referenced read-only as namespace `prop`; root `\|prop:Group\|prop:root` | `/Game/Animation/C02/Rigging/SK_C02_CombineBody_Clothes_05` |

The wire ID `prop` is the existing second-subject slot; its input is C02.
`SK_C01_CombineBody_Clothes_12` is incompatible with this older animation's
1399-bone rig (it needs `upperarm_r_twist_0001`): that refusal is valid and is
not a program defect. The bone-table-matching `SK_C01_Clothes_09_All` is the
C01 target used below.

## Current findings

| ID | Status / blocking | Evidence and completion condition |
| --- | --- | --- |
| R-001 | Verified closed / no | SessionAndTimeIdentity rejects stale-session frames/removes and preserves current-session admission. |
| R-002 | Verified closed / no | A strictly increasing serial identifies evaluation; source time may reverse or repeat. Real peer times 1→120→60→60 report first/forward/backward/hold. |
| R-003 | Verified closed / no | OwnershipRenegotiation and the evidence show driving=true, exit=preview_active and restored_to_reference_pose=false after takeover/re-init. |
| R-004 | Verified closed / no | DriverExitOwnership restores a previously undriven target to reference pose; fixture disconnect/world cleanup checks pass. |
| R-005 | Fixed, pending review / yes | The necessary set (all-LOD positive skin weights and ancestors) is the negotiation's target: every necessary bone is driven by one declared bone, and a bind difference on a bone that deforms nothing no longer vetoes the declaration. Untrimmed C01 and C02 declarations negotiate and stream against the production meshes (numbers below). |
| R-006 | Verified closed / no | CommandLineArguments and an actual peer with Times before Remove/abslog pass; Maya exits normally and applies four frames plus the removal. |
| R-007 | Fixed, pending review / yes | The real C01 animation plus the real C02 second character now stream one session: four common frames at 1→120→60→60, independent removal of the C02 subject, a C01-only sample frame, a disconnect that restores both targets, a renegotiation that refuses the wrong skeleton and accepts the right one, and two BaseColor captures of a real sampled frame with both targets framed (numbers and images below). |
| R-008 | Fixed, pending review / yes | The mapping is one scope-wise complete candidate relation with a unique assignment; competing sources, competing targets and several covering assignments are refused in every declaration order. The review's `Joint`/`Joint1` → weighted `Joint12` case is refused in both orders on both subject targets, a uniquely assignable rename chain resolves identically in both sibling orders, and an exact name still wins over a rename. |

## What changed

`MtoUMultiSubjectProtocol.cpp`'s negotiation was rebuilt around the product's
accepted rules (the prototype stays independently installable, so the rules are
adapted in place and checked by the mirrored contract cases below):

* The negotiation target is the necessary set only - every target bone with a
  positive skin weight in any LOD plus its ancestors, the same set
  `MtoUCharacterComposition` resolves. Coverage and the rest check apply to it;
  a bind difference on a target bone that deforms nothing is accepted and
  reported instead of rejecting the declaration (R-005).
* Every child of one mapped parent resolves as one scope over its complete
  candidate relation - exact names first, then the importer's rename forms (a
  numeric suffix, or `_` plus 32 hexadecimal digits) for target bones no exact
  name owns - and the targets are driven by the single assignment of sources to
  targets that covers them. Two sources claiming one target, one source with
  several targets the relation does not settle, and several covering
  assignments are `skeleton_mismatch` naming the target, its parent and the
  declared bones, in every declaration order (R-008). The rename applicability
  stays the prototype's documented wire rule: a rename-shaped target is a
  candidate in the mapped parent scope, an exact name always wins, and a
  contested candidate set is refused rather than guessed. The product's
  stricter "duplicated short name only" precondition is deliberately not
  imported, because this prototype's own uniquely-assignable rename case and
  the review's counterexample both exercise unique short names; the two rules
  coincide on every real rig sampled here (all rename targets of the real
  C01/C02 declarations come from duplicated short names).
* The rest check keeps its constant-root-frame form and its prototype noise
  bounds (5 cm, 10°, 5% scale) but only over the necessary set, and a mapped
  bone outside it only has to stay invertible for the projection. The frame
  projection is the product's bind/current conversion unchanged.

New regression tests: `RenameCandidateContracts` (the review's contested-rename
case on both subject targets, both orders; a unique-assignment rename chain in
both sibling orders; two indistinguishable sources for two renamed targets in
both orders; exact-name priority) and `NonEssentialBindDifferences` (a
non-essential same-name bind difference accepted and excluded from the
rig-agreement numbers, the same difference on a necessary bone refused).
The existing fixture assertions - necessary-bone coverage, ignored export
branches, weighted-leaf motion, rename reporting, ownership, session identity,
Sequencer takeover and cleanup - are unchanged and still pass.

`RealAssetPair` gained the R-007 lifecycle evidence and picture: the lifecycle
phases replay recorded real Maya samples instead of a synthesized hold, the
second subject is removed alone while the first keeps streaming, a disconnect
restores both targets, and the BaseColor captures fit both targets' world
bounds with a geometric framing check. One documented reposition of the
disposable second target is captured separately, because the supplied scene
references both rigs at the world origin and the pair would otherwise render
superimposed.

## Executed verification and evidence limits

All raw evidence is local under the Git root's sibling directory
`../.tmp/mtou-issue54/fix-20261010/`. Environment: Windows 10, Maya 2024,
UE 5.7.4, ToolsLab plus the one-shot Backups-content shell project.

| Check | Result / evidence |
| --- | --- |
| Prototype pure Python unittest discovery | 23/23 passed. |
| Maya 2024 host test | 104 checks, zero failures; `host.json`. |
| UE 5.7.4 prototype suite (ToolsLab, real mayapy fixture peer) | 15 tests performed, all passed: the 13 previous checks plus `RenameCandidateContracts` and `NonEssentialBindDifferences`. RealMayaPeer streamed four frames with Remove=2 and Times=1,3,2,2. `RealAssetPair` is the opt-in real-asset check and reported "not requested" inside this run. |
| Real pair, Backups-content shell, real mayapy peer | `RealAssetPair` passed; `realpair/c01-c02/`. C01 declared=1399 driven=1399 required=647, C02 declared=1502 driven=1502 required=187, source_only=0 and undriven=0 for both; necessary-set rest deviation 0.0000848 cm / 0.0389° (C01) and 0.0000088 cm / 0.0549° (C02). These are the review's own required-only comparison values, now produced end to end by the untrimmed declarations. |
| Real sampled frames | Four applied frames at Maya times 1→120→60→60, directions first/forward/backward/hold; per-object maximum projected-pose delta 0.00151 (character) and 0.00146 (C02). 428 real Morph values compared on the production meshes, maximum difference 1.2e-6. Three declared C01 curves the target has no Morph for are reported as `source_only_curve_names`. |
| Real lifecycle | `realpair/c01-c02/lifecycle/`: frame 1 (time 120) carries both subjects, the C02 `remove` reports prop=disabled while character=applied and the prop returns to its reference pose, frame 2 (time 1) streams the character alone with a 54.76 cm component-space pose change between the two real samples, the disconnect ends the session and both targets report `exit=reference_pose, restored_to_reference_pose=true`. |
| Renegotiation | The character's declaration against the C02 target is refused with `skeleton_mismatch` naming the target's required bone; the correct declarations are accepted on a fresh session and a frame applies to both. |
| BaseColor evidence | `basecolor-c01-c02.png` (scene placement; the two rigs coincide at the scene's own origin, so the pair renders superimposed) and `basecolor-c01-c02-separated.png` (the documented reposition of the disposable second target, the pair side by side). Both show one real Maya-sampled frame (time 120) and pass the geometric framing check that each target's bounding sphere fits the view cone completely. |
| Source preservation | The run's own digests verify the two Skeletal Mesh files, the supplied Maya scene and both packages are unchanged and not dirtied; the peer evidence verifies the referenced rig files are unchanged. No original scene or mesh was saved. |

Known limits of this round: the production C01 meshes reference an old Anim
Blueprint whose KawaiiPhysics nodes need a plug-in this host does not have, so
loading the user's asset logs property errors; `RealAssetPair` marks those
expected so its own assertions stay visible, and the boundary is recorded here.
The supplied rig roots carry the scene's import convention (a 90° root
rotation), so the disposable-world pair inherits it through the placement
anchor; the pose evidence is the per-bone projection delta, not the picture's
world orientation. The BaseColor images show the sampled frame at one instant;
continuous playback performance, Maya 2022, product caches and product
integration were not tested, and the picture is not a deformation-quality
claim. The pure-Python, Maya host and ToolsLab suites were executed for this
delivery; the earlier fixture results in `review-20261010/` remain valid for
the closed findings.

## Next action

Review the delivery against R-005/R-007/R-008: the negotiation rewrite in
`composite/MtoULiveLink/prototypes/multi-subject/unreal/MtoUMultiSubjectPrototype/Source/MtoUMultiSubjectPrototypeEditor/Private/MtoUMultiSubjectProtocol.cpp`,
the two new regression tests, the real-pair evidence above and the BaseColor
images. Keep #45 character parts distinct from #54 independent-character
pairing. Product integration, the multi-object cache and the product ownership
UI stay with #47. The prototype's cache remains a design for a shared time
lattice/range/fps and atomic readiness/failure.

## Record index

The [source archive](issue-54-comment-archive.json) preserves exact live report
IDs, bodies and timestamps of the reports replaced by the current handoff,
including dev-02 and review-02. One machine-specific asset path in an archived
body was redacted to the user-relative `MtoU_tmp/` form before publication, as
the archive rule's private-path inspection requires; the live report keeps its
original text. Earlier implementation and review evidence
remains historical (`review-20261009/`, `review-20261010/`, `fix-20261008/`,
`fix-20261009/`, the shared `fix-20261008/backups-copy/` host), not a
contradictory current acceptance claim. This delivery's raw evidence lives in
`fix-20261010/`. This is an open-issue delivery record, not an archival-closeout
claim.
