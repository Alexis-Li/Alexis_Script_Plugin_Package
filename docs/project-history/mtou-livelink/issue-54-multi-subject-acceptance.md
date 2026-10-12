# Issue #54: independent prop and second-character preview

Current state: **2026-10-12, fixes delivered for review-04; issue remains open**.
Latest implementation: the commit that carries this record on
`codex/mtou-preview-workflow` (not pushed; the review base is the parent commit).
The last reviewed baseline is still `5add03e155c8ea8eb8b98765c9627cd309696887`.
R-001/R-002/R-003/R-004/R-006 remain verified closed; R-005/R-008/R-009 are
fixed and await re-verification; R-007 is re-run against the real inputs.
The [current handoff](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-5883765955)
owns the next action.

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

## Findings and what this delivery changes

| ID | Status after this delivery | Change and completion condition |
| --- | --- | --- |
| R-005 | Fixed, awaiting re-verification | Mapping runs in two phases per mapped parent scope. The *necessary* targets - every positive-weight bone of any LOD plus its ancestors - decide acceptance: coverage, hierarchy, ambiguity and bind veto there. Everything outside that set is resolved afterwards, keeps every necessary assignment, cannot veto the declaration, and stays at its reference pose with the reason in `optional_ambiguities` when it is not settled uniquely. Completion condition: the review's optional-ambiguity counterexample is accepted in both orders while an ambiguity inside the necessary range still refuses. |
| R-008 | Fixed, awaiting re-verification | The rename forms now require the product's applicability condition: the declared short name must occur more than once in the whole declaration (`SourceCount > 1`, the same count the product negotiator takes). A unique short name is refused; the same name repeated in another declared branch is accepted. Valid rename tests build their duplicated names in distinct branches, like the real C01/C02 rigs. |
| R-009 | Fixed, awaiting re-verification | Both real targets are spawned under *one* common placement anchor and the world lift is gone. The opt-in real-asset run compares landmark bones' actual world positions with Maya's own evaluated world poses of the same bones (`--probe-bones`), placed by that anchor: the import convention, the scene motion and the common placement are each applied exactly once. An always-run fixture test covers the same contract with non-zero separation, a 90-degree root convention and a moving reference parent. |
| R-007 | Re-run on the real inputs, review pending | Full C01/C02 declarations, four frames, Morph values, independent removal, disconnect restore and renegotiation pass; the world oracle and the fresh BaseColor images are in the evidence below. |
| R-001/R-002/R-003/R-004/R-006 | Verified closed | Session identity, time direction, ownership re-init, reference restore and Times/Remove parsing keep passing in the suite. |

### R-005: the necessary relation decides, the rest can only be added

`FMtoUMappingPass` is now run twice: first with the necessary target bones as the
only candidates, then with the remaining ones. The second pass starts from the
first pass's assignment (`MtoUMultiSubjectProtocol.cpp`, `RunMappingPass`), so a
bone the necessary phase mapped keeps its target, and an optional candidate can
never change which declared bone drives a necessary target. Ambiguities of the
second phase are collected into `FMtoUNegotiationMap::OptionalAmbiguities`
(sorted, reported per subject as `optional_ambiguities`) instead of refusing the
declaration, and the bones involved are the ones listed in `undriven_bones`.
`NecessaryScopeMapping` covers the review's counterexample, the optional
candidate beside a unique necessary assignment, and the still-refused
necessary-range ambiguity.

### R-008: the importer's rename means a repeated short name

`DescribeNegotiationMismatch` counts every declared name of the declaration and
`BuildScope` requires that count to exceed one before a renamed target is a
candidate; the count is the whole declaration's, not the parent scope's, exactly
like the product negotiator's `SourceNames`. A refusal now explains the
applicability condition (`its short name occurs once in this declaration, so it
is not an import rename`), so a unique short name is refused with the uncovered
target named and no rename applied. The suite's rename fixtures build their
duplicated names in distinct declared branches - the distribution the real rigs
have - and the shared Maya fixture recipe keeps the Unreal target's own name,
because a three-bone fixture rig cannot repeat a short name without inventing a
second branch. The real C01/C02 declarations repeat 5 (C01) and 19 (C02) short
names, and every rename the previous round applied used a name with a count of at
least two, so the strict condition leaves that pairing unchanged.

### R-009: one anchor, and an oracle that is not the projection

The Maya probe can now record named bones' evaluated **world** poses in the same
UE rows as a root pose (`--probe-bones id=bone,bone`, `sample_world_bones`), and
the real-asset test
* samples every held source time, so each held frame has Maya's own evaluation of
  that time;
* compares, per held frame, the actual `GetBoneTransform(..., RTS_World)`
  position of the character's `head`/`pelvis` and the prop's landmark against
  `Anchor.TransformPosition(MayaWorld)` - the projection takes no part in the
  expectation;
* asserts the head stands above the pelvis by a body's length, so a pair rotated
  by a re-added convention cannot pass;
* keeps the probe-vs-stream relation check and the per-bone/anchor residuals as
  separate evidence.

Only bones inside the target's necessary set are used as landmarks, because that
is the set whose bind the negotiation verified - the premise of comparing
component space with the scene.

## Evidence of this delivery

Windows 11, Maya 2024 (`mayapy`), Unreal 5.7.4. All checks were executed against
the working tree described above; nothing is claimed from an earlier round.

| Check | Result |
| --- | --- |
| Prototype Python discovery (`python -m unittest discover -s <prototype>/maya/MtoUMultiSubjectPrototype/tests`) | 23/23 passed |
| Maya host checks (`mayapy tests/maya_host_multi_subject_tests.py`), after the fixture-recipe change | 104 checks, 0 failures |
| ToolsLab prototype suite `MtoUMultiSubjectPrototype` (NullRHI, scripted peer) | 17 tests, 0 failures (`FixtureAndAnchorRules`, `NecessaryScopeMapping`, `RenameCandidateContracts`, `WorldSpacePlacement`, `NonEssentialBindDifferences`, `RealMayaPeer`, …) |
| Isolated reviewer diagnostics `Issue54Review` (review-03's own `ReviewDiagnostics.cpp`, unchanged, on the fixed source) | `NonRequiredAmbiguity` **Succeeded**: both declaration orders are accepted and the same necessary declaration without the optional branches is accepted too. `UniqueNameRename` **Succeeded**: the unique short name is refused with `target bone 'Joint12' below 'Chest' deforms the mesh but the declaration drives no bone for it; declared 'Joint' (its short name occurs once in this declaration, so it is not an import rename) look like importer renames of it and were not applied`. `AnchorConvention` **Failed** as written: it evaluates the expression the old test built, which the fixed run no longer constructs; the placement contract it stood for is covered instead by the world oracle above and by the always-run `WorldSpacePlacement` test |
| The same isolated host, prototype suite | 17 tests, 0 failures (the reviewer's diagnostics and the prototype tests share the module) |
| Real C01 animation + C02 target, Backups shell, non-NullRHI (`RealAssetPair`) | Success. Declarations unchanged in coverage (C01 1399 declared / 647 required, C02 1502 / 187); max bone residual 0.001510; max anchor residual 0.000523 |
| World oracle, t=120 and t=1 | character head 4e-6 cm and 6e-6 cm, character pelvis 2e-6 cm and 1e-6 cm, prop pelvis 0 cm against `Anchor * MayaWorld`; 9 landmark samples across the three held source times |
| Real lifecycle | Independent removal, reference restore and renegotiation pass; the two real samples pose the character differently by 54.763369 cm |
| BaseColor captures, re-shot | `basecolor-c01-c02.png` and `basecolor-c01-c02-separated.png`: the pair stands upright on the ground with both objects fully framed (the second image moves the disposable prop anchor aside to show both objects separately) |
| Repository gates | `python tools/validate_repository.py` ok; `python -m unittest discover -s tests` 37 tests, 36 passed and 1 skipped because PowerShell 7 (`pwsh`) is not installed on this host - the skip is the pre-existing environment guard, not this change |

Local evidence: `../.tmp/mtou-issue54/fix-20261012/` (`ue-suite/`, `realpair/`,
`diagnostics/`, `host.json`, `world.json`, build and run logs). It is local
scratch and not claimed remotely available. The real assets and scenes were
opened read-only; the run records their digests and that no package was dirtied.

## Boundaries

Maya 2022, sustained playback, product cache and product integration were not
tested and are not prototype acceptance requirements. The prototype drives only
the necessary bones plus the optional bones it can settle uniquely; a bone left
unmatched stays at its reference pose and is reported. The rename rule is
exercised at the declaration level in the suite and end-to-end by the real
C01/C02 run; the shared three-bone fixture rig does not carry a duplicated short
name. Product v9 integration, the multi-object cache and the ownership UI remain
with #47.
