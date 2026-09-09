# Cached exit and explicit refresh review acceptance

Date: 2026-09-09. Independent review of Issues #38 and #39 at
`4378d2756d255b4bfc3cad4e9ba7ed83a9b55af7`, including #38's `0021971`
and #39's `4378d27`. Both fixes pass the reviewed acceptance scope.
No implementation changes, commits, or pushes were made by this review.
This record is the only repository addition. After review, the user explicitly
requested publication and closure; the acceptance conclusions were posted to
[Issue #38](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/38#issuecomment-5598703021)
and [Issue #39](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/39#issuecomment-5598703411),
and both issues were closed as completed on 2026-09-09.

## Issue #38: natural editor refresh

`MtoULiveLink.Source.CacheClearNaturalRefresh` places the Binding Actor in
the actual Editor world, requires Level viewports, disables their underlying
Realtime preference, and exercises cached enter/clear and revision-mismatch
recovery. Its three pose observations read displayed bone transforms only:
root translations (101,102,103), (111,112,113), and (121,122,123).
The waits do not manually update the source, tick Live Link, tick animation,
or refresh bone transforms. Control-plane pumping precedes the pose sends;
the normal editor loop must deliver and evaluate the subsequent poses.

The new test and `CacheClearRestoresLivePreview` passed together under
D3D12 (SM5), RenderOffScreen, on stock UE 5.7.4. This closes the previous
manual-animation-pump evidence gap. The existing companion test supplies
the initially-on/off preference, repeated-switch, disconnect, recoverable
failure, and active-shutdown matrix. Both also passed within the full suite.
The developer's reported negative-control mutation was inspected as a claim
in the issue; it was not independently repeated in this review.

## Issue #39: isolation and replacement boundary

The scope guard in `MayaRefreshPeer` now owns the Maya process, Live Link
source, and test world/context on normal and early-return paths. The full
suite ran in one fresh UE process with both Maya peers opted in. It included
`MayaRefreshPeer`, `RefreshEndsSession`, `Details.RefreshClick`, both cache
clear tests, `CacheSessionReconnect`, and `MayaCacheReconnect`; every entry
was Success. The previously reported cross-test Binding Actor contamination
did not recur.

The unchanged W1/W2 integration coverage still tests real admitted old live
and cache input while worker cleanup is deferred, synchronous publication
rejection, and frozen in-flight replay. It also covers Animation and Model
failed-refresh readiness and workflow/Morph renegotiation. The Details
handler test is treated as handler coverage, not a physical click.

## Actual two-host Details smoke

Hosts: the already-open Backups editor on stock UE 5.7.4 and Maya 2022.4,
using the supplied C01 character and Clothes09 outfit. The installed plugin
Source tree matched the reviewed repository Source tree by file hashes;
the editor log records that installed copy being built at startup. No
assets or levels were saved, and the existing Maya scene was not replaced.

Actual screenshot-directed clicks on the Actor Details **Refresh Preview**
button produced the following observations:

| Starting state | Operation and observed result |
| --- | --- |
| Animation, Connected | Refresh succeeded; UE displayed a new Generated Preview and Disconnected. Maya reported transport interruption and remained disconnected until explicit reconnect. Animation reconnect restored the live Driver workflow. |
| Model, Transfer BS on, Connected with partial Morph coverage | Refresh succeeded; UE replaced the generated mesh and showed Disconnected. Maya reported transport interruption. Explicit Model reconnect succeeded and reported partial Morph coverage again. |
| Model, Connected | A temporary unknown garment slot was injected in memory without property-change notification. The log confirmed the session was still Connected before the real button click. Refresh failed, cleared Generated Preview, restored the Driver display, and disconnected both hosts. |
| Animation, Connected while Preview readiness was failed | A new Animation connection succeeded against the Driver. Clicking Refresh again with the same invalid slot failed and disconnected both hosts, retaining the Driver fallback and no Generated Preview. |
| Disconnected, original slot setting restored | Clicking Refresh rebuilt a valid Generated Preview. Final explicit Animation reconnect succeeded. |

The failure input used the built-in Unreal Python console only to prepare
`DriverGarmentSlotOverride = ["MtoUReviewMissingSlot"]` with
`PropertyAccessChangeNotifyMode.NEVER`; Refresh itself was always a real
Details button click. This isolates Refresh termination from the separate
property-edit invalidation path. The original empty array was restored and
read back as `[]`. A subsequent public-property observation logged
`Disconnected None True`: disconnected status, absent Generated Preview,
and the displayed skeletal mesh equal to the Binding Driver.

An attempted Python read of the private readiness property failed because
it is not exposed to Python. Exact Error readiness is therefore supported
by the successful C++ integration assertions; the UI smoke independently
supports the diagnostic, absence of generated output, and Driver fallback.

The character has existing Morph-name differences: Maya reported three
Clothes09 curves absent in Unreal, Unreal reported five eyelash curves
absent in Maya, and 14 bone renames. The Model UI accurately reported
partial coverage after explicit renegotiation. This review does not certify
full artistic Morph coverage for that asset; deterministic accepted-curve
behavior remains covered by the integration fixtures.

## Verification

| Check | Independent result |
| --- | --- |
| Stock UE Development Editor `Build.bat UnrealEditor Win64 Development ToolsLab.uproject -WaitMutex -NoHotReloadFromIDE` | Succeeded; no compiler/linker warnings in this build |
| `Automation RunTests MtoULiveLink`, NullRHI, both real Maya peers opted in | 58/58 Success: 51 without warnings, 7 with warnings; 0 failed/notRun |
| D3D12 RenderOffScreen: `CacheClearNaturalRefresh+CacheClearRestoresLivePreview` | 2/2 Success, both with expected revision-mismatch warnings; JSON confirms D3D12 (SM5) |
| Maya pure `unittest discover`, owning tests directory | 121/121 passed |
| Maya 2022.4 `mayapy .../tests/maya_host_tests.py` | 19/19 passed |
| Scoped Ruff on `maya_refresh_peer.py` and `maya_reconnect_peer.py` | Passed |
| `generate_unreal_corpus.py --check` | Passed |
| `package_unreal_plugin.py MtoULiveLink --engine 5.7 --json` | Dry-run passed, 34 files |
| `package_maya_tool.py MtoULiveLink --json` | Dry-run passed, 1 file |
| `git diff 4440d10..HEAD --check` | Passed |

The full-suite warnings comprise deliberately rejected cache input and
missed-window cases, plus pre-existing no-world-context warnings in the
Lifecycle/Readiness fixtures. The reviewed refresh tests reported no errors.
Both Maya result files reported `ok: true`; refresh reported actual transport
loss, no automatic reconnect, and a new explicit session. Reconnect reported
the same retained cache, one capture, four applied frames in each session,
and cache cleanup.

Raw local evidence is under the temporary run directory named
`mtou-review-3839-20260909-112847`: `report/index.json`,
`d3d12-report/index.json`, `editor.log`, `d3d12.log`,
`maya-refresh-result.json`, and `maya-result.json`. UI observations are
recorded above; the review conversation contains the actual screenshots.
These generated artifacts are not committed.

Repository-wide checks were not run because no shared tooling, layout, or
rules changed. UE-generated ToolsLab configuration residue was removed and
the tracked configuration restored. The temporary garment override was
restored, a valid preview rebuilt, and both hosts left connected in Animation.
No remaining blocker was found for #38 or #39 within this scope.
