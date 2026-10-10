# Issue #54: independent prop and second-character preview

Current review: **2026-10-10, changes required; issue remains open**.
Implementation and reviewed baseline:
`07c9039defaaecb79274f38d1e53b4be15473d89` (local, not pushed).
R-001/R-002/R-003/R-004/R-006 pass within the tested prototype scope.
R-005 and R-007 remain open; R-008 is a newly reproduced mapping defect.
The [formal review](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-6095144149) gives the findings and completion conditions;
the [current handoff](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/54#issuecomment-5883765955)
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
as the second character in place of an unavailable standalone heroine arm.
The user accepted the located C01 animation scene. Missing arm assets are no
longer an acceptance blocker, and the earlier generated Box023 rig does not
substitute for this requested second character.

Inputs below are relative to the user-supplied `MtoU_tmp` asset directory:

| Role | Maya input / root | UE candidate |
| --- | --- | --- |
| C01 animated character | `C01_Body_IdleStand02_ChangeClothes.ma`, referencing `SK_C01.ma`; root `\|SK_C01:Group\|SK_C01:root` | `/Game/Animation/C01/Rigging/SK_C01_Clothes_09_All` |
| C02 independent character | `C02/SK_C02_05MH.ma`, added as a real reference in an unsaved session; root `\|prop:Group\|prop:root` | `/Game/Animation/C02/Rigging/SK_C02_CombineBody_Clothes_05` |

The wire ID `prop` is the existing second-subject slot; its input is C02.
Both references were read successfully at 30 fps in centimetres. Samples at
1, 120, 60, 60 show changing C01 bone digests and 24, 18, 25, 25 non-zero
Morph values out of 78. Repeating frame 60 produces the same digest. C02's
1502-bone pose and 235 Morph values remain at its supplied binding state.
No synthetic animation or forced Morph override was used for these samples.
This establishes meaningful source animation, not successful UE playback.

`SK_C01_CombineBody_Clothes_12` is incompatible with this older animation's
1399-bone rig: it needs `upperarm_r_twist_0001`, which that source does not
provide. That refusal is valid and is not counted as a program defect. The
bone-table-matching `SK_C01_Clothes_09_All` supplies the useful comparison below.

## Current findings

| ID | Status / blocking | Evidence and completion condition |
| --- | --- | --- |
| R-001 | Verified closed / no | SessionAndTimeIdentity rejects stale-session frames/removes and preserves current-session admission. |
| R-002 | Verified closed / no | A strictly increasing serial identifies evaluation; source time may reverse or repeat. Actual peer times 1→3→2→2 report first/forward/backward/hold. |
| R-003 | Verified closed / no | OwnershipRenegotiation and recorded state now show driving=true, exit=preview_active and restored_to_reference_pose=false after takeover/re-init. Exit restoration is separately reported. |
| R-004 | Verified closed / no | DriverExitOwnership restores a previously undriven target to reference pose; fixture disconnect/world cleanup checks pass. |
| R-005 | Confirmed defect / yes | All-LOD required-bone coverage now works, but bind rejection still includes non-required matching branches. Scope strict validation and driven mapping to required dependencies, report ignored branches, and preserve refusal of required-bone conflicts. Re-run full, untrimmed C01/C02 declarations. |
| R-006 | Verified closed / no | CommandLineArguments and an actual peer with Times before Remove/abslog pass; Maya exits normally, applies four fixture frames and removes the second subject. |
| R-007 | Acceptance gap / yes | Real C01+C02 init fails before a session or any applied frame. After R-005/R-008, demonstrate common-frame streaming, isolated remove/disconnect, renegotiation, reversible ownership and BaseColor views of actual sampled frames. C02 may stay at its real binding pose; do not require a fabricated arm asset. |
| R-008 | Confirmed defect / yes | Same-parent source Joint and Joint1 both qualify for weighted target Joint12. Current suffix matching accepts the first source and silently ignores the second; reversing declaration names changes the source selected. Resolve complete candidate relationships and reject ambiguity in both orders. |

### R-005: unnecessary branches still reject compatible dependencies

The protocol maps all matching source/target bones, then checks the rest pose
of every mapped row (`MtoUMultiSubjectProtocol.cpp`, mapping at 737–837 and
rest validation at 909–968). The required mask is used only for coverage.
This is not the accepted #55 rule that non-required redundant branches do not
cause failure, despite the delivery's claim to implement that contract.

| Actual declaration and target | Complete declaration | Diagnostic required-only declaration |
| --- | --- | --- |
| C01, 1399 source bones → Clothes_09_All | Rejects, names `ik_hand_l`; maximum over the complete mapping is 60.0554 cm / 72.4319° | 647/647 required bones accepted; 0.000085 cm / 0.038857° |
| C02, 1502 source bones → CombineBody_Clothes_05 | Rejects, names `SM_C02_hair28_jnt__4`; maximum rotation difference 103.0229° | 187/187 required bones accepted; 0.000009 cm / 0.054898° |

The C02 named bone has target index 1179 and `required=false` when the
unchanged UE implementation reads all LOD skin weights and ancestors. In the
comparison, only declaration membership is reduced; retained bind rows,
target assets and tolerances are unchanged. These are direct calls to the
actual negotiation function. They establish false rejection from irrelevant
branches, not repaired end-to-end streaming. Increasing the 10° tolerance is
not an appropriate fix. Add both a non-required bind-conflict positive case
and a required bind-conflict refusal case.

### R-008: two sources can silently compete for one renamed target

In a disposable character fixture, rename weighted target `Head` to `Joint12`.
Rename its declared source to `Joint`, then add same-parent `Joint1` with the
same bind. The actual negotiation returns an empty error and mapping
`first=3, second=-1`. Swapping the two source names produces the same index
mapping, so the target is now driven by the other name. Both rejection
assertions fail. The candidate search filters `ClaimedBy[Candidate]` before
testing suffixes (line 794), hiding the already claimed candidate from the
second source. The existing later ambiguity check therefore cannot see it.
The fix must consider complete candidate relationships under the mapped
parent and exact-name priority. Preserve unique numeric/hash suffix successes.

## Executed verification and evidence limits

All new raw evidence is local under the Git root's sibling directory
`../.tmp/mtou-issue54/review-20261010/`. It is not remotely published.

| Check | Result / evidence |
| --- | --- |
| Prototype pure Python unittest discovery | 23/23 passed; `python-tests.log`. |
| Maya 2024 host test | 104 checks, zero failures; `host.json`. |
| UE 5.7.4 ToolsLab and Backups-shell builds | Succeeded/up to date; `build.log`, `build-backups.log`. |
| Unmodified UE prototype suite with actual Maya fixture peer | Report says 13 Success, but RealAssetPair explicitly did no work without its opt-in arguments. Twelve tests executed; `ue-suite/report/index.json`. RealMayaPeer used four frames, Remove=2 and Times=1,3,2,2 before later arguments. |
| Real C01 animation plus real C02 reference probe | Passed; `c01-c02-animation-probe.json`. Bone digests change with animation and repeat identically at a held frame. |
| Complete real pair, original receiver | `c01-c02-matched/`: Clothes_09_All + C02 body, Sequence flag and Times=1,120,60,60; init refuses the C01 non-required bind difference, zero applied frames, no established session. `c01-c02/` records the valid Clothes_12 mismatch; `c02-body/` independently records C02's false refusal. |
| Supplementary UE diagnostics | `diagnostics-final/report/index.json`: RequiredMask succeeds; RenameAmbiguity has two failed refusal assertions. `required-only/report/index.json` contains the accepted required-only comparisons. Candidate probing also has missing-object and absent KawaiiPhysics/old-ABP load errors; neither diagnostic aggregate is represented as a passing suite. |
| Source preservation | SHA-256 before/after matches for the C01 animation, its rig reference, C02 rig and the two initially selected body mesh files; `inputs-before.json`, `inputs-after.json`. RealAssetPair also checks its selected mesh/scene digests and package dirty states. No original Maya scene or mesh was saved. |

The reviewer added `ReviewDiagnostics.cpp` and `ReviewRealCandidates.cpp` only
to a scratch copy of the UE plugin. All 18 pre-existing UE source files match
the reviewed checkout byte-for-byte. `real_pair_peer.py` selects the real C02
file for the default second rig and supplies its root; it uses the committed
sampler, conversion, negotiation, transport and lifecycle. Its SHA-256 is
`a51efdd96e41246eeb2fbb44f5a1c8cd977d17c7478f9b5b85da454f3b522699`.
Required-only filtering occurs solely in the diagnostic comparison, not in
the real-pair run. UE process exit zero does not override a failed Automation
report. An earlier reviewer-test crash was corrected before the recorded
diagnostics; it is not a product finding.

The previous delivery's BaseColor image at
`../.tmp/mtou-issue54/fix-20261009/real-asset/final-body/real-body/basecolor-real-body.png`
was visually inspected. It mainly shows the ground/background and provides no
legible complete pair for comparison. The assertion checks file existence,
and the displayed pose is synthesized rather than sampled from the real Maya
animation. It does not establish the requested visual acceptance. This round
does not claim a new rendered comparison: negotiation fails before streaming.
No actual post-application stretching or cross-talk was reproduced.

Fixture tests still establish independent identities, duplicate Root/Shared
isolation, same-time admission, moving-parent rules, no-driver restoration,
Sequencer takeover and world cleanup within their controlled setup. They are
not substituted for the missing real-pair evidence. Maya 2022, sustained
playback performance, product caches and product integration were not tested.
No repository-wide checks were rerun for this documentation-only review.

## Handoff and retained evidence

The implementer should fix R-005/R-008, preserve the passed R-003/R-006 tests,
and repeat the complete real pair with no declaration trimming. Capture
actual sampled-frame BaseColor views with both actors visibly framed, plus
per-object numeric and ownership evidence. The prototype's cache remains a
design for a shared time lattice/range/fps and atomic readiness/failure;
implementation and a user-visible product ownership UI stay with #47.

The [source archive](issue-54-comment-archive.json) preserves exact live report
IDs, bodies and timestamps before the current handoff is replaced, including
dev-02 and review-02. The prior current versions remain recoverable from this
archive at local commits `6c2c4ee998bd71952f835d9e3e17267bacf5458a` and
`7efa9f80b577a5f181558f5e295ab45f70b89ed3`. Earlier implementation and review
evidence remains historical, not a contradictory current acceptance claim.

This review retains `review-20261010/` host evidence, its real-peer adapter and
diagnostic source/project for the implementer's unresolved checks. Earlier
`review-20261009/`, `fix-20261009/` evidence and the shared
`fix-20261008/backups-copy/` host remain in use for comparison/reproduction.
Publication drafts, API payloads and this round's report-writing helper are
removed after verified publication. Host-generated tracked-tree config output
is restored to the initial clean state. This is an open-issue review, not an
archival-closeout claim. There is no new branch, PR, push or product-code edit;
the acceptance record and exact source archive are committed locally because
remote publication of repository changes was explicitly prohibited.
