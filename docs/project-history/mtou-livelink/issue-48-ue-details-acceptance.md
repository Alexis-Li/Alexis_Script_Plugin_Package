# Issue #48: UE Binding Actor Details acceptance

Date: 2026-09-24. Scope: the Unreal Editor half of
[Issue #48](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/48)
on branch `codex/mtou-preview-workflow`. The Maya panel was accepted earlier
and is unchanged. No protocol, mesh matching, cache, or asset-serialization
behavior changed; only the Details presentation and interaction did.

## Field and action map

| Display | Existing source | Behavior |
| --- | --- | --- |
| Preview state and build stage | `AMtoULiveLinkActor::GetPreviewReadiness()` | Read only; Preview failure stays independent of connection. |
| Connection state | `AMtoULiveLinkActor::GetConnectionStatus()` | Read only; the source status is classified for the Chinese label, and the full message stays in diagnostics. |
| Current display | `AMtoULiveLinkActor::GetDisplayTarget()` | Read only. |
| Primary and Additional Parts | `GetBinding()->SkeletalMesh` and `AdditionalParts` | Actual asset names, count, disabled markers, and the full path on hover; the Binding reference stays separate from Primary. |
| Binding and generated Preview mesh | Existing `Binding` and transient `GeneratedPreviewMesh` actor properties | Native property rows in Preview Controls; picker, browser navigation, and read/write behavior stay with the original properties. |
| Refresh and Delete | Existing `HandleRefreshPreviewClicked` and `NotifyGeneratedPreviewDeleted` | Existing eligibility and operation paths retained; Delete stays disabled until a usable Generated Preview exists. |
| Garment slot override | `UMtoULiveLinkBinding::DriverGarmentSlotOverride` | Advanced row reports automatic mode or the current names and points at the Binding asset; the UI never picks a section. |
| Full diagnostics | Readiness diagnostics and summary, character part diagnostics, Model diagnostics, detailed connection status | Exact duplicate strings are collected once; the original text is selectable and copyable in a height-limited field. |

Four native categories replace the previous Preview/Diagnostics split:
`MtoU · 运行状态`, `角色组成`, and `预览控制` open by default;
`高级设置与诊断` starts collapsed. The Editor module registers those four
categories in the `MtoU` category-filter section, so the toolbar filter shows
the curated groups instead of the retired names. All user-facing strings use
`FText`/`LOCTEXT`; asset names, paths, internal codes, and raw logs stay in
their original form.

The known two-region ambiguity maps to a short Chinese summary, and the section
IDs and triangle counts are parsed from the current raw message; unknown
failures get a generic Chinese summary while the complete original text stays
available. Expanding or collapsing a group reads state only: it does not
refresh, reconnect, or modify assets.

## Verification

| Check | Result |
| --- | --- |
| `Build.bat UnrealEditor Win64 Development` against the isolated host project (stock UE 5.7.4) | Succeeded |
| `Automation RunTests MtoULiveLink.Editor`, NullRHI | 31/31 Success, including `Editor.DetailsStatus` and `Editor.Details.RefreshClick` |
| `Automation RunTests MtoULiveLink`, NullRHI | 78/78 Success, 0 failed, 0 not run |
| Host Details walkthrough, stock UE 5.7.4 with the Backups C01 and C02 Binding assets | Four groups render as designed; the `MtoU` filter shows only the curated groups |
| Real Refresh failure (C01 Binding without a Preview Static Mesh) | `预览：预览生成失败` and `连接：未连接` render as separate states with a Chinese summary and one next step |
| Expanded `高级设置与诊断` on that failure | Override row, summary, cause, next step, the original English message, and `复制完整诊断`; the clipboard received `Select both Driver Skeletal Mesh and Preview Static Mesh.` |
| Fresh Binding actor with no diagnostics | The advanced group shows only the override row; no blank summary, log, or copy row appears |
| Content widths ~500, ~430, and ~350 px (floating Details tab) | No truncated status, asset row, or preview action; summaries and the raw log wrap, and the filter buttons wrap to further rows |
| Hover on the Primary row | Tooltip returns the full `/Game/Animation/C01/Rigging/SK_C01_CombineBody_Clothes_12.SK_C01_CombineBody_Clothes_12` path |

`Editor.DetailsStatus` asserts the ambiguity path with the verbatim production
message: `预览生成失败` plus `未连接`, the `服装区域匹配冲突` summary, the
parsed `区段 1：86 个三角面` and `区段 4：38 个三角面` candidates, the
`0.0200` threshold inside the copyable raw text, and single collection of a
duplicated summary. It also asserts the Chinese fallback for an unknown error
and that a new build does not retain the previous error.

## Limits

- The available Backups Binding assets (C01 and C02) carry no Preview Static
  Mesh, so the real garment-ambiguity failure could not be reproduced in the
  host. The host pass exercised a different real failure (preflight without a
  Preview mesh) end to end; the ambiguity rendering is covered by
  `Editor.DetailsStatus` against the production message rather than by a host
  screenshot.
- Connected, validating, interrupted, and building states were verified through
  the automation state matrix, not by a live Maya session in this pass. The
  earlier Maya-side acceptance remains the reference for the connected workflow.
- Local evidence (not committed) lives in the `issue48-evidence` folder of the
  scratch directory beside this repository: the walkthrough screenshots,
  `issue48-editor-tests3.log`, `issue48-full-tests.log`, and the exported
  automation reports for both runs.
