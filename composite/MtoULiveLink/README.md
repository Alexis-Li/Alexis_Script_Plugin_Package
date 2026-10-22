# MtoU_LiveLink

[中文说明](README_CN.md)

MtoU_LiveLink connects Maya to Unreal Editor on the same computer so you can
preview character animation, BlendShapes, and garment changes in Unreal while
working in Maya. It supports real-time preview and cached animation playback
without creating animation or preview assets.

## Compatibility

- Windows 64-bit
- Autodesk Maya 2024 (current workflow); Maya 2022.4 remains supported
- Stock Unreal Editor 5.7.4
- Topia Engine 5.7.4 (plugin build and editor loading verified)
- Editor use only; Play In Editor (PIE) is not supported.

Compatibility with other third-party Unreal Engine 5.7 builds is not guaranteed.
Install the Maya and Unreal components from the same release.

## Installation

The component paths below are relative to this product directory.

1. Copy the whole `maya/MtoULiveLink/scripts/` directory — `MtoULiveLink.py` and
   its `mtou_livelink/` package — into a Maya scripts directory, or run
   `MtoULiveLink.py` directly in Maya's Python Script Editor. The package must
   stay beside the entry file; copying the single file alone no longer runs.
2. Copy `unreal/MtoULiveLink/` to `<Project>/Plugins/MtoULiveLink/`.
3. Compile the Unreal project. For Topia Engine, use the instructions below.
4. Enable **Live Link** and **MtoU_LiveLink**, then restart Unreal Editor.

Maya and Unreal components upgrade and roll back as a pair: both hosts speak
the same protocol version and refuse a mixed pair before touching a scene. To
upgrade, replace both components and reconnect; to roll back, restore the
previous release's `scripts/` directory (entry file **and** package) and the
previous plugin build, then reconnect. Close the current session or restart
Maya before replacing files.

### Topia Engine 5.7.4

**Recommended: double-click wizard (Windows, Chinese UI)**

Close Unreal Editor and ensure the source plugin is installed at the project's
`Plugins/MtoULiveLink` directory.
Double-click `tools/build_mtou_topia_gui.cmd`, select the project `.uproject`, then
the company engine's `Engine/Binaries/Win64/UnrealEditor.exe`. Check the displayed
paths and click OK. Wait for the success message before opening the project.
On failure, share the console error and `Build log` path with a technical teammate.
If a compiler or SDK is missing, ask a technical teammate to configure
it or supply a plugin already compiled for the same company engine version.
After compilation, the tool replaces the target plugin's build outputs.
To copy the wizard separately, keep `build_mtou_topia_gui.cmd`,
`build_mtou_topia_gui.ps1`, and `build_mtou_topia.ps1` in the same folder.

**Command-line alternative**: Close Unreal Editor. Set `TOPIA_ENGINE_ROOT` to the directory containing
`Engine` and `ATHENA_UPROJECT` to your target `.uproject`, then run this command
from the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./tools/build_mtou_topia.ps1 `
  -EngineRoot $env:TOPIA_ENGINE_ROOT `
  -ProjectFile $env:ATHENA_UPROJECT `
  -Apply
```

Omit `-Apply` to check the paths and preview the installation first.

## Prepare your character

- Maya must contain the bones that deform the enabled Unreal meshes and their
  complete ancestor chains, including intermediate groups. Names and parent
  relationships must match. Duplicated Maya short names may map to imported
  numeric or 32-digit hash suffixes under the same parent; the connection reports
  those mappings. Unused exported branches do not prevent connection, but one
  Unreal bone is driven by exactly one Maya bone: rename a Maya bone when two of
  them publish the same name under the same parent.
- For garment preview, prepare a **Preview Static Mesh** aligned with the
  Primary Driver garment. Keep garment and non-garment geometry in separate
  imported material slots.
- To preview BlendShapes, use matching Maya BlendShape and Unreal Morph Target
  names and enable **Transfer BS** (传递 BS) in Maya.

### Separate character parts

When one character is delivered as several Skeletal Meshes, assign the garment
source as the Primary Driver and add the other meshes under **Additional Parts**
on the Binding. All enabled parts pose and display under one connection:

- Only the Primary Driver supplies garment resolution, weight transfer, and
  **Preview Morph transfer**.
- Give each part a name, its Skeletal Mesh, and an **Enabled** flag. All meshes
  must share one **Skeleton** asset. Shared bones needed for skinning across
  any LOD, including their ancestors, must agree in hierarchy and reference pose.
  A part's own secondary bones receive Maya animation even when the Primary
  does not contain them. Disabled parts add no bone requirements.
- After adding clothing and secondary bones, import the new clothing, update
  the enabled parts, and reconnect. Update older meshes only if their own bind
  pose or weights changed. A missing required Maya bone or an incompatible
  shared bone blocks connection and names the affected part and bone.
- A Morph Target that only a part owns still streams, and a name owned by
  several parts receives the same value on each of them.
- Adding, removing, enabling, disabling, or replacing a part ends the current
  connection, so reconnect in Maya afterwards. A generated garment preview
  survives. Run **Refresh Preview** after changing the Primary
  Driver, the Preview Static Mesh, the **Driver Garment Slot Override**, or
  relevant imported data.
- Renaming a part or reordering the list needs no reconnect.

## First connection

1. In Unreal, create an **MtoU_LiveLink Binding** asset, assign its
   **Primary Driver Skeletal Mesh**, and add any **Additional Parts**.
2. Drag the Binding asset from the Content Browser into the level to create
   its Binding Actor. Its Details panel opens **MtoU · 运行状态** (separate Preview
   and connection states plus the next step), **角色组成** (Primary and expandable
   Additional Parts), and **预览控制** (Binding and generated mesh asset fields,
   Refresh, Delete, and cache playback). Expand **高级设置与诊断** for the full
   selectable report; **复制完整诊断** copies it, including the linkage and
   scene-reference details, for a problem report. An empty diagnostic does not
   occupy the daily view.
   The connection reads 未连接 before the first session, 已连接 while streaming,
   and 连接已中断 after a session ends; reconnect in Maya after an interruption.
3. In Maya, run `MtoULiveLink.py`, select the deformation root joint, and click
   **Set Character** (设置角色). Switch between **Animation** (动画) and **Model**
   (模型) at the top. Below are Connection Controls (set character, connect,
   disconnect, and connection status), Scene Info (root, outfit, and frame rate
   on the left; bone/BlendShape counts and cache on the right; a long name is
   shortened, hover it for the full name), Preview & Playback (preview mode,
   transmission cap, and cache actions in Cached Playback), Camera Linkage (one
   status line, the linkage buttons, and the scene-reference window), Tools
   (Display and duplicate-bone selection), and Diagnostics (status and
   诊断详情, whose **复制完整诊断** copies the whole report). The warning switch
   sits below Diagnostics.
4. Select **Animation** (动画) and click **Connect** (连接); the indicator turns
   green. Pose, scrub, or play in Maya to preview the result in Unreal. **Cap**
   (上限) on the preview row limits the real-time transmission rate. A disabled
   button carries its reason as a tooltip. Repeated warnings for the same
   session appear once; new errors still interrupt.

## Animation preview

Use **Animation** for real-time character preview. For a captured animation
range, choose **Cached Playback** (缓存播放) in Maya. Leave **Custom Capture Range**
(自定义捕获范围) off to use the current playback range, or turn it on and enter
integer start and end frames. Click **Capture and Upload** (捕获并上传), then wait
for **Ready**. The range includes both endpoints and the scene frame rate is
fixed when capture starts; capture does not change Maya's playback range.
Single frames and negative start frames are supported.

In the Unreal Binding Actor's **预览控制**, check the source range, rate, current
applied source frame, and cache state. **播放** starts from the first frame, and
**再次播放** replays a cache retained by a stop or a completed run. **暂停** holds
the current pose and the cache for as long as you need, and **继续** carries on
from that pose without replaying the paused time. **停止** ends the attempt and
keeps both the cache and the held pose. To inspect one frame, type an integer
Maya source frame beside **定位** and click it: Unreal displays that frame and
holds it paused. Invalid or overflowing integers are rejected in the row.
Only frames this capture sampled are accepted; an unsampled frame is
refused, and the row names the requested frame and the sampled range. **循环**
wraps from the last frame back to the first and shows the current round; turning
it off completes the current round. Locating and looping reuse the uploaded
cache. Natural completion holds the final frame. To prepare a new range in Maya,
click **捕获并上传** again; the old UE playback ends before sampling begins.
Switch Maya back to **实时预览** to clear the UE cache and resume live poses.
After a connection loss, reconnect and use **上传保留缓存** if a compatible
complete local cache remains. Maya's status line shows capture, upload, Ready,
playback, pause, seek, loop, stop, and failure; a run completed after a seek or
a loop round is reported as a playback segment rather than a full replay.

A capture is limited to 20,000 frames or 1 GiB; use a shorter range if you reach
that limit. Invalid ranges fail before sampling. Sampling or upload failures
return to real-time preview and show the reason. A playback failure retains
the complete UE cache for another attempt.

## Camera linkage preview

Use this workflow to check Maya animation under the Unreal sequence camera and
its timeline. It is optional: realtime preview, Cached Playback and garment
comparison work without it.

1. In Unreal, select the Binding Actor, click **配对** under **镜头联动** (the row
   then reads 已配对 Maya 角色), and open the Level Sequence you want to follow.
2. In Maya, connect the character as usual and click **进入联动** on the main
   panel's **镜头联动** card (or click **进入联动** in the Unreal Details panel;
   Maya follows either one). Maya stops its own playback, follows the sequence
   time and the evaluated camera, and answers with the complete pose of the
   character.
3. Both hosts show one status line, for example
   镜头联动中 · Unreal 控制时间 · 同步正常. When Maya cannot keep up, the line
   says Maya 跟不上 with the lag; pause the Sequencer to let Maya catch up, or
   close programs that load the machine. A lagging pose is never reported as
   synchronised. The evaluation, sample and revision read-back stays in Maya's
   诊断详情 and Unreal's **高级设置与诊断**, ready for **复制完整诊断**.
4. **返回实时** restores Maya's timeline, view and gate and keeps Maya paused
   while Unreal keeps its takeover; user playback resumes in Maya. **结束预览**
   also returns the original writers to the sequence and stays available after
   **返回实时** until it does. Both buttons work from either host.

While linkage is active, a camera edit, a pause, a cut or an explicit jump
cancels the old target; a continuously moving camera never cancels the answer
already on its way. Unreal refuses a target whose writers cannot be isolated
safely, and names the reason, instead of pausing the whole sequence. Cached
Playback must be cleared before entering linkage. A failed restore appears on
both hosts only when it happens (恢复失败 in Maya, 恢复未完成 in Unreal):
open Maya's 诊断详情 or Unreal's **高级设置与诊断** for the resources that were
not restored, restore them by hand (for example unlock the camera or reset the
time range in Maya, or check the track in the Sequencer), or reopen the level. Closing or switching the Level
Sequence, closing its level or deleting the paired actor ends linkage: Unreal
returns the writers and Maya restores itself. Opening or creating another Maya
scene ends the connection after linkage restored the scene it was started in.
If the Maya camera linkage drives is deleted or its camera shape replaced while
linkage runs, Maya refuses the next target and writes no other camera; end the
preview and start linkage again to use the current camera.

## Scene reference in Maya

Unreal can export an explicit level scope as static reference geometry so Maya
shows the same surroundings. The reference is a display aid: no preview
workflow requires it, and the handoff never delivers image data.

1. In Unreal, choose the level and the sublevels to include under **场景参考**
   and run the export. Unreal reports the result, for example 已导出 152 个对象，
   可在 Maya 导入, and says when a requested sublevel was not loaded. Pair the
   target first (see camera linkage) so the reference lines up with it.
2. In Maya, click **打开场景参考窗口** on the main panel's **镜头联动** card. The
   window shows the source level and sublevels, the size (for example 152 个对象，
   约 120 万三角面) and whether the handoff is 可导入, 已导入 (with the time) or
   已过期 and needs a new export from Unreal. Click **导入/更新参考**: Maya
   validates the handoff and its size budgets before it touches the scene,
   imports into its own container, and states the result in one sentence.
   Discarded nodes, rollback and leftovers are listed only when an import fails.
   The handoff identity, digest, space revision and budgets are in 诊断详情.
3. **移除参考** deletes only that container; **恢复上次更新** finishes or
   discards an interrupted update.

The reference is placed in the same frame as the character and the linked
camera, so it follows a moved or rotated target. The placement is the paired
target's position when Unreal exports. Export and import again after moving or
rotating the target, and for any reference exported by an earlier version; if
the status line says the manifest carried no anchor, the reference sits at the
Maya origin until you do.

## Garment model preview

1. On the Unreal Binding, assign the garment **Preview Static Mesh**.
2. Click **刷新预览** on the Binding Actor and wait for the Preview status to
   show ready, warning, or failed. A failure names the cause in Chinese while
   connection remains a separate status. Expand **高级设置与诊断** to inspect and
   copy the original error; the rest of the Driver character remains visible.
3. In Maya, select **Model** (模型), confirm the outfit and **Transfer BS**
   setting, then click **Connect**.
4. Pose the character in Maya to inspect garment deformation in Unreal.
5. In the Binding Actor's **预览控制**, click **显示原始服装** and **显示生成服装**
   to compare the same pose under the same actor transform, camera, and light.
   The body and enabled Additional Parts remain visible. This control requires
   a ready Generated Preview; if disabled, read the Preview status or hover the
   button for the next step. 当前显示：原始角色 after a failed or invalidated
   Preview is for inspection and is not a Model comparison result.

## Common problems

| Problem | What to do |
| --- | --- |
| Maya reports an incomplete MtoU_LiveLink install | A part of the `scripts` directory is missing; the message names it. Copy the whole `scripts` directory (`MtoULiveLink.py` and the `mtou_livelink` folder) into one Maya scripts directory again, then restart Maya. |
| Skeleton mismatch | Start from the reported root cause: the first unmapped Maya path and parent, the blocked Maya and unreached Unreal counts, and any suggested import rename. When the details report that one required bone is claimed by two Maya bones, rename one of them in Maya. Then check the selected Maya root, the hierarchy, and whether the Unreal Driver mesh is up to date. |
| An Additional Part is rejected | The diagnostics name the part and the reason. Check the shared **Skeleton** asset, the reported required bones and their parent chains in Maya, and the shared reference poses. If skin weights cannot be read, rebuild or reimport the mesh with CPU skin data available. |
| Preview refresh fails | Check mesh alignment and separate garment/body material slots. Correct the reported issue and refresh again. |
| Auto garment selection rejects an intentionally reduced mesh | Inspect the source for duplicates or mixed garment/body slots. If the reduction is intentional, set **Driver Garment Slot Override** on the Binding to the garment's separate slots, then refresh and inspect the result. |
| BlendShapes do not appear | Check **Transfer BS**, matching names, the selected outfit, and the connection diagnostics. Refresh the Model preview after changing its source assets. |
| Connection ends after a change | Reconnect after changing the outfit, mode, **Transfer BS**, or the character parts. Refreshing also disconnects the session. After changing or reimporting the Primary Driver or the Preview Static Mesh, or changing **Driver Garment Slot Override**, refresh the Model preview and reconnect. Undo/Redo of these changes follows the same rule. |

Keep the Binding Actor and its level loaded while previewing; deleting the actor
or unloading its level ends the connection.

## Further reading

- [Changelog](CHANGELOG.md)
- [Development and acceptance records](../../docs/project-history/mtou-livelink/README.md)
