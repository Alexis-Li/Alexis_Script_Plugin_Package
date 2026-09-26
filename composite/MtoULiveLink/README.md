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

1. Copy `maya/MtoULiveLink/scripts/MtoULiveLink.py` to a Maya scripts directory,
   or run the file directly in Maya's Python Script Editor.
2. Copy `unreal/MtoULiveLink/` to `<Project>/Plugins/MtoULiveLink/`.
3. Compile the Unreal project. For Topia Engine, use the instructions below.
4. Enable **Live Link** and **MtoU_LiveLink**, then restart Unreal Editor.

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
   Refresh, Delete, and cache playback). Expand **高级设置与诊断** for the full selectable and
   copyable raw error; an empty diagnostic does not occupy the daily view.
3. In Maya, run `MtoULiveLink.py`, select the deformation root joint, and click
   **Set Character** (设置角色). Switch between **Animation** (动画) and **Model**
   (模型) at the top. Below are Connection Controls (set character, connect,
   disconnect, and connection status), Scene Info (root, outfit, and frame rate
   on the left; bone/BlendShape counts and cache on the right), Preview &
   Playback (preview mode, transmission cap, and cache actions in Cached Playback),
   Tools (Display and duplicate-bone selection), and Diagnostics (status and
   诊断详情). The warning switch sits below Diagnostics.
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
applied source frame, and cache state. Click **播放** to start, **停止** to hold the
last applied pose, and **再次播放** to play the retained cache from its start.
Natural completion holds the final frame. To prepare a new range in Maya, click
**捕获并上传** again; the old UE playback ends before sampling begins. Switch Maya
back to **实时预览** to clear the UE cache and resume live poses. After a connection
loss, reconnect and use **上传保留缓存** if a compatible complete local cache remains.
Maya's status line shows capture, upload, Ready, playback, stop, and failure.

A capture is limited to 20,000 frames or 1 GiB; use a shorter range if you reach
that limit. Invalid ranges fail before sampling. Sampling or upload failures
return to real-time preview and show the reason. A playback failure retains
the complete UE cache for another attempt.

## Garment model preview

1. On the Unreal Binding, assign the garment **Preview Static Mesh**.
2. Click **刷新预览** on the Binding Actor and wait for the Preview status to
   show ready, warning, or failed. A failure names the cause in Chinese while
   connection remains a separate status. Expand **高级设置与诊断** to inspect and
   copy the original error; the rest of the Driver character remains visible.
3. In Maya, select **Model** (模型), confirm the outfit and **Transfer BS**
   setting, then click **Connect**.
4. Pose the character in Maya to inspect garment deformation in Unreal.

## Common problems

| Problem | What to do |
| --- | --- |
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
