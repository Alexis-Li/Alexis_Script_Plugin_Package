# Issue #48: UE Binding Actor Details acceptance

Date: 2026-09-24; review follow-up applied and re-verified 2026-09-28. Scope: the
Unreal Editor half of
[Issue #48](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/48)
on branch `codex/mtou-preview-workflow`. The Maya panel was accepted earlier
and is unchanged. No protocol, mesh matching, cache, or asset-serialization
behavior changed; only the Details presentation and interaction did.

## Field and action map

| Display | Existing source | Behavior |
| --- | --- | --- |
| Preview state and build stage | `AMtoULiveLinkActor::GetPreviewReadiness()` | Read only; Preview failure stays independent of connection. |
| Connection state | `AMtoULiveLinkActor::GetConnectionStatus()` | Read only; the source status is classified for the Chinese label, and the full message stays in diagnostics. |
| Session lifecycle | `AMtoULiveLinkActor::GetLinkSessionState()` | Locally tracked `Idle`/`Streaming`/`Ended` state, never serialized and never sent on the wire. It separates 未连接 from 连接已中断, which the status string cannot: every ended session also reports `Disconnected`. |
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
the curated groups instead of the retired names. Every string this panel adds
goes through `FText`/`LOCTEXT`, including the Additional Parts `未指定网格` and
`{0}（已停用）` labels and the cache summary skeleton; asset names, frame
numbers, paths, internal codes, and raw logs stay in their original form.

`ClassifyIssue` is the single ordered problem list that both the status summary
and the next step read: Preview build failure, then a blocking connection
failure, then a non-blocking Preview quality warning, then the link's own phase.
A failed Preview stays the first blocker, and a Warning Preview whose connection
reports `Error:` or `Preview morph mismatch` therefore shows the connection
failure in both the summary and the next step instead of a quality-warning hint.

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
| `Automation RunTests MtoULiveLink`, NullRHI | 80/80 Success, 0 failed, 0 not run (2026-09-28 rerun) |
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

The review follow-up is covered by these automation paths rather than by an
injected status string:

- `Editor.DetailsStatus` walks the lifecycle the source produces (Connected +
  Streaming, Disconnected + Ended, reconnect) and asserts `已连接`,
  `连接已中断`, that an interruption never reads as `未连接`, that it stays
  neutral rather than an error, that the cached presenter follows it, and that a
  session which never streamed stays `未连接` after it ends. It also asserts the
  priority ladder with exact next-step text for Warning × `Error:`,
  Warning × `Preview morph mismatch`, Warning × Connected, Warning × Validating,
  and Error Preview × `Error:`.
- `Editor.Details.RefreshClick` runs a real Live Link source and socket client:
  a fresh actor reads `未连接`, the negotiated session reads `已连接`, the
  Details-driven refresh termination reads `连接已中断`, the reconnected session
  reads `已连接` again, and a peer socket close reads `连接已中断`.
- `Source.SessionTerminationBoundary` asserts the same actor-local state on real
  events: `Idle` before the first session, `Streaming` while connected, `Ended`
  after the ADR-0002 reimport termination, and `Streaming` again after reconnect.

## Limits

- The available Backups Binding assets (C01 and C02) carry no Preview Static
  Mesh, so the real garment-ambiguity failure could not be reproduced in the
  host. The host pass exercised a different real failure (preflight without a
  Preview mesh) end to end; the ambiguity rendering is covered by
  `Editor.DetailsStatus` against the production message rather than by a host
  screenshot.
- The 2026-09-28 follow-up verified the new states through a real source/socket
  session in automation, not through a host details screenshot. The earlier
  host walkthrough remains the reference for the rendered layout.
- Local evidence (not committed) lives in the `issue48-evidence` folder of the
  scratch directory beside this repository: the walkthrough screenshots,
  `issue48-editor-tests3.log`, `issue48-full-tests.log`, and the exported
  automation reports for both runs. The follow-up logs are
  `mtou-issue48-fixes/full-tests2.log` in the same scratch directory.
