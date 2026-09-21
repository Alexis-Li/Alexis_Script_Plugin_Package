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

- Use a Maya deformation skeleton that matches the Unreal **Primary Driver
  Skeletal Mesh**, including bone names and parent relationships. Intermediate
  groups between joints also participate in matching. A short name that exists
  in several Maya branches may still connect when the Unreal import renamed the
  duplicate with a numeric or 32-digit hash suffix below the same parent; the
  connection reports every such mapping. The plugin does not retarget; update
  the Unreal asset after adding or removing bones in Maya.
- For garment preview, prepare a **Preview Static Mesh** aligned with the
  Primary Driver garment. Keep garment and non-garment geometry in separate
  imported material slots.
- To preview BlendShapes, use matching Maya BlendShape and Unreal Morph Target
  names and enable **Transfer BS** (传递 BS) in Maya.

### Separate character parts

When one character is delivered as several Skeletal Meshes, assign the mesh with
the complete deformation hierarchy as the Primary Driver and add the others
under **Additional Parts** on the Binding. All enabled parts then pose and
display as one character under one connection:

- The Primary Driver alone defines the skeleton baseline and supplies garment
  resolution, weight transfer, and **Preview Morph transfer**. Body, face, and
  BlendShape preview keep working through it.
- Give each part a name, its Skeletal Mesh, and an **Enabled** flag. A part must
  share the Primary Driver's **Skeleton** asset, map its bones by name and
  parent path onto the Primary, and match its reference pose for the bones that
  deform the part and their ancestors across all LODs. A part may use fewer
  bones and different geometry; a bone the Primary
  does not have is rejected with the part and bone name.
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
   its Binding Actor. Use the **MtoU** section in Details for plugin controls.
   Its **Character parts** row lists the resolved composition and reports any
   part that cannot join the character.
3. In Maya, run `MtoULiveLink.py`, select the deformation root joint, and click
   **Set Character** (设置角色). Check the detected Display controller, outfit,
   scene frame rate, and transmission cap.
4. Select **Animation** (动画) and click **Connect** (连接). Pose, scrub, or play
   in Maya to preview the result in Unreal.

## Animation preview

Use **Animation** for real-time character preview. For a captured animation
range, set the Maya playback range, choose **Cached Playback** (缓存播放), and click
**Capture and Play** (捕获并回放). Playback starts after capture and upload finish.

A capture is limited to 20,000 frames or 1 GiB; use a shorter range if you reach
that limit. Capture or upload failures return to real-time preview and show the
reason. If playback fails, you can retry the retained cache or leave cached mode.

## Garment model preview

1. On the Unreal Binding, assign the garment **Preview Static Mesh**.
2. Click **Refresh Preview** on the Binding Actor and check the result.
   **Modified parts** lists the material slots replaced by the preview;
   the rest of the Driver character remains visible.
3. In Maya, select **Model** (模型), confirm the outfit and **Transfer BS**
   setting, then click **Connect**.
4. Pose the character in Maya to inspect garment deformation in Unreal.

Keep **Transfer BS** enabled when evaluating the model. Turning it off provides
bone-only diagnostics and does not validate BlendShape deformation. Garments
with no BlendShapes can be previewed normally with bone-driven deformation.

Inspect any yellow **Warning** before accepting the result. An **Error** blocks
Model connection until **Refresh Preview** succeeds. Refresh can take several
seconds on larger meshes; wait for it to finish.

Use **Delete Preview** to remove the generated preview and restore the Driver
mesh display. Configure lighting and shadow settings on **SkeletalMeshComponent**.

## Common problems

| Problem | What to do |
| --- | --- |
| Skeleton mismatch | Start from the reported root cause: the first unmapped Maya path and parent, the blocked Maya and unreached Unreal counts, and any suggested import rename. Then check the selected Maya root, the hierarchy, and whether the Unreal Driver mesh is up to date. |
| An Additional Part is rejected | The diagnostics name the part and the reason. Check that it shares the Primary Driver **Skeleton** asset, that its reported bones exist in the Primary under the same parent, and that its reference pose matches. |
| Preview refresh fails | Check mesh alignment and separate garment/body material slots. Correct the reported issue and refresh again. |
| Auto garment selection rejects an intentionally reduced mesh | Inspect the source for duplicates or mixed garment/body slots. If the reduction is intentional, set **Driver Garment Slot Override** on the Binding to the garment's separate slots, then refresh and inspect the result. |
| BlendShapes do not appear | Check **Transfer BS**, matching names, the selected outfit, and the connection diagnostics. Refresh the Model preview after changing its source assets. |
| Connection ends after a change | Reconnect after changing the outfit, mode, **Transfer BS**, or the character parts. Refreshing also disconnects the session. After changing or reimporting the Primary Driver or the Preview Static Mesh, or changing **Driver Garment Slot Override**, refresh the Model preview and reconnect. Undo/Redo of these changes follows the same rule. |

Keep the Binding Actor and its level loaded while previewing; deleting the actor
or unloading its level ends the connection.

## Further reading

- [Changelog](CHANGELOG.md)
- [Development and acceptance records](../../docs/project-history/mtou-livelink/README.md)
