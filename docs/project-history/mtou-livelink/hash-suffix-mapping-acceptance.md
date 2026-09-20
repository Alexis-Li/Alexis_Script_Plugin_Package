# Hash-suffix skeleton mapping acceptance (Issue #46)

Status: implemented and host-verified on 2026-09-20 on local `main`. The
[Unreleased changelog](../../../composite/MtoULiveLink/CHANGELOG.md#unreleased)
owns the user-facing entries; this record keeps the technical evidence.

Date: 2026-09-20. Follow-up to the
[complete-skeleton connection and preview acceptance](complete-skeleton-acceptance.md).

## Correction

An Unreal import renames a duplicated Maya short name either with the numeric
suffix the engine appends or with the complete short name followed by `_` and a
32-digit hexadecimal hash. Only the numeric form was accepted. In the supplied
C02 character the head branch stores `MHHead:spine_04` as
`spine_04_5d859dce24654c43b1b653def8d6278f`, so that bone failed first, its 792
descendants were skipped as blocked, and the unreached Unreal nodes were then
listed under `Extra in Unreal`, which readers correctly took as "Maya lacks
these bones".

The negotiator now accepts both rename forms under exactly the rules that
already applied to the numeric suffix:

| Condition | Requirement |
| --- | --- |
| Name shape | Complete Maya short name plus digits, or plus `_` and exactly 32 hexadecimal digits |
| Parent | Already mapped Unreal parent of the same branch |
| Maya name | Duplicated short name in the published skeleton |
| Candidates | Exactly one unused Unreal bone |
| Precedence | An exact name under the mapped parent always wins |

Partial original names, a different separator, another hash length,
non-hexadecimal digits, a unique Maya name, a different parent, several
candidates, and target reuse stay blocking. No rig, Skeletal Mesh, Skeleton
asset, protocol field, or protocol version changed.

## Diagnostics

The validation response leads with the root cause of the first unmapped bone:
its complete Maya path, the mapped Unreal parent it had to match, the blocked
Maya descendant and unreached Unreal counts, and every import-rename shaped
candidate with the reason the strict rules did not apply it. Unmatched Unreal
bones behind an unmatched ancestor are listed as `Unreached in Unreal` instead
of `Extra in Unreal`, so one broken ancestor no longer claims that Maya lacks
its descendants, while confirmed extra bones stay listed. The Maya
`SKELETON_MISMATCH` advice now points at those details and asks for a
re-export or re-import only after the assets are confirmed inconsistent.

## Automation

`MtoULiveLink.Negotiation.ConnectionCompatibility` keeps its original fixtures
and adds the C02-shaped head branch: with a valid hash the branch maps
completely and reports one mapping, and with a 31-digit hash the ancestor is the
only confirmed extra bone while its descendants are unreached and the root
cause names `root/spine_02/joints_grp/spine_04` below `joints_grp`. The same
test covers both rename forms, same-parent ambiguity for hash/hash and
numeric/hash candidates, wrong parents, unique Maya names, invalid or
wrong-length hashes, prefix-similar names, target reuse, and real missing or
extra bones.

- Stock Unreal Editor 5.7.4 build of both plugin modules succeeded.
- All 67 `MtoULiveLink` Automation tests passed on the rebuilt plugin, including
  the character-part composition and Preview suites that share the negotiator.
- Maya pure tests 140/140, repository validator, and repository tests passed.

## Offline cross-check

The rules were reproduced independently in Python over the C02 data: the Maya
standalone inventory (1502 published nodes below `|Group|root`) against the
1502-bone reference skeleton of `SK_C02_CombineBody_Clothes_05` yields 1502
mapped nodes, 19 hash renames, and zero missing, ambiguous, parent-mismatched,
or unreached nodes. The 19 Unreal names include the reported
`spine_04_5d859dce24654c43b1b653def8d6278f`, `clavicle_l_d334a92079e82c0021e10535bf76649b`,
and `clavicle_out_l_53c5c41168c676cc0a908cd2c105d37a`. This cross-check is a
transcription of the rules, so it corroborates rather than replaces the host run
below.

## Real host acceptance

The opt-in character harness started a disposable stock Unreal Editor 5.7.4 on
`/Engine/Maps/Entry` with the rebuilt plugin and a Maya 2024 standalone peer
running the production capture, controller, and sender. The fixture used the
user's `/Game/Character/C02_112/DA_C02_MtoUBinding` unchanged: Primary Driver
`SK_C02_CombineBody_Clothes_05` (1502 bones) with its two enabled Additional
Parts `SK_C02_Head` and `SK_C02_Hair_01`.

- Animation handshake published all 1502 nodes, and the connection reported
  exactly the 19 expected hash renames.
- Four poses were accepted: body `spine_03`, head-branch `spine_05`, garment
  sleeve `SM_C02_Clothes02_yixiu_jnt_9`, and the reported
  `FACIAL_C_12IPV_NeckB2` (Unreal index 481, parent 474). Every pose published
  1502 transforms with a maximum world position error at or below
  `2.1e-13 cm` and a rendered-component error at or below `2.1e-13 cm`.
- An explicit Maya disconnect and reconnect republished 1502 bones and the same
  pose accuracy, so the mapping survives a second handshake.
- Render screenshots of all four poses were captured; no visible skeleton
  difference remains, and `FACIAL_C_12IPV_NeckB2` is no longer reported as
  missing from Maya.

The harness now runs the Animation phases and the reconnect for a Binding that
has no Preview Static Mesh instead of failing the run, and the Maya peer gained
a `reconnect` action that uses the production disconnect/connect path.

## Limits

- The Model connection with a Generated Preview was not run for this character.
  The C02 Binding has no Preview Static Mesh and the project contains no C02
  garment Static Mesh, so no Generated Preview can be built for it; the C01
  Binding does have a Preview, but no matching Maya source scene is available
  (the available C01 scenes publish 1399 and 1412 nodes against its 1472-node
  Driver). Model coverage rests on the owning Automation tests in the same
  build, not on a real-character run.
- The connection reports a Morph difference warning (Maya BlendShapes with no
  Driver Morph Target) and the project assets load with an existing
  `SK_C02_Head` LOD warning. Neither is a skeleton difference and neither was
  introduced here.
- No user scene, Binding, Skeletal Mesh, Morph asset, or DNA was modified,
  saved, or uploaded; the acceptance ran against the original Binding. Only the
  plugin binaries of the user-designated test project were rebuilt, and the
  temporary duplicate Binding created there for inspection was deleted.
- Local fixtures, logs, screenshots, and peer JSON stay outside the checkout
  under the temporary test-data directory; the original scene hashes are
  unchanged.
