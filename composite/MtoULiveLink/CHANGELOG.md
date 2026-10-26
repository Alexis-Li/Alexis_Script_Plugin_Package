# Changelog

## Unreleased

### Upgrade notes

- Update the Maya and Unreal components together: protocol v10 refuses a mixed
  pair before it touches either scene. (#57)
- Install the Maya component by dragging its `install.mel` into a Maya viewport.
  The first time, it offers to delete the copied `MtoULiveLink.py` and
  `mtou_livelink` from your Maya scripts directory; your MtoU shelf button keeps
  working. (#78)
- Cached Playback is now played, stopped and located from the Unreal Binding
  Actor's **预览控制**; Maya only captures and uploads the cache. (#49, #50)

### Added

- Camera linkage (**镜头联动**): pair the Maya character with its Unreal target to
  follow the Level Sequence's time and camera in Maya, and leave through
  **返回实时** or **结束预览** from either host. (#57)
- Scene reference: export a level from Unreal as static reference geometry and
  import it into Maya, lined up with the character; a failed update keeps the
  previous reference. (#57)
- **暂停**/**继续**, **定位** to a sampled source frame, and **循环** for Cached
  Playback in the Binding Actor. (#50)
- **Custom Capture Range** in Maya, or capture the current playback range. (#49)
- **显示原始服装** and **显示生成服装** switch the outfit in the same Model session
  to compare garment deformation under the same pose. (#51)
- Additional Parts with their own secondary bones stream in real time and in
  Cached Playback. (#55)

### Changed

- Redesigned Maya panel and Chinese Unreal Details groups; the same warning
  interrupts only once per connection. (#48)
- Panels show only state, actions and problems; detailed values are in
  **诊断详情** and **高级设置与诊断**, copied by **复制完整诊断**. (#71)
- Long root, outfit, frame-rate and cache names no longer widen the Maya panel;
  the full text is in the tooltip. (#57, #71)
- README Common problems explain how to fix a frequent **Maya 跟不上** during
  linkage by turning off unused plug-ins' Auto load. (#76)

### Fixed

- Closing the **MtoU 场景参考** window from its title bar no longer crashes Maya. (#80)
- Linkage could show a stale pose at the target time. (#57)
- Bone mapping depended on Maya's sibling order; duplicate short names now
  connect in any order, and a real ambiguity names the bones involved. (#55)
- A lost connection reads as **连接已中断** instead of **未连接**. (#48)

### Removed

- The **打开镜头联动窗口** button and its window; their details are in
  **诊断详情**. (#71)

## 0.5.0 - 2026-09-22

Milestone accepted after practical use and tagged
`composite-mtou-livelink-v0.5.0`; no installation packages were published.

### Added

- **Additional Parts** in a Binding: the existing Skeletal Mesh becomes the
  Primary Driver, and parts such as a separate head move with it, show their
  own Morphs and appear in Model preview. An incompatible part is rejected with
  the part, bone or pose difference. (#45)
- Maya streams the BlendShapes of a separately skinned part, such as a Head
  mesh, together with the body's. (#45)
- Maya 2024 is the current workflow; Maya 2022.4 remains supported.
- A double-click Chinese wizard, `tools/build_mtou_topia_gui.cmd`, builds the
  plugin for Topia Engine.
- Skeleton mismatch details name the first unmapped Maya bone, its parent and
  possible import renames. (#46)

### Changed

- Adding, removing or switching a part ends the connection but keeps a
  generated garment Preview; renaming or reordering parts changes nothing. (#45)
- README Common problems explain how to use **Driver Garment Slot Override**
  for an intentionally reduced model that automatic garment detection
  rejects. (#40)

### Fixed

- Undo and redo of a Binding edit now update every actor that uses it. (#45)
- Bones the importer renamed with a 32-digit hash suffix map like numeric
  suffixes. (#46)
- Saved levels with Additional Parts that use Post Process animation
  blueprints failed to open correctly.
- Split Head/Hair characters were rejected when an unrelated, unweighted
  skeleton branch had a different reference pose.
- Characters with very small non-zero bone scales, such as pupils, were
  rejected on connect. (#44)
- Joints under intermediate transform groups were left out of the capture; a
  missing joint is now reported by its path. (#44)
- Cached Playback uploads were refused after reconnecting. (#37)
- Refreshing the Preview while connected could apply old frames to the new
  mesh; Refresh now ends the connection first. (#39)

## 0.4.0 - 2026-09-03

### Upgrade notes

- Update the Maya and Unreal components together: the protocol moves from v2
  to v6, and an older peer is refused with a version mismatch.
- The Binding's Skeletal Mesh field is now **Driver Skeletal Mesh**; existing
  Bindings keep working.
- Play In Editor is not supported.

### Added

- Model workflow (**模型**): **Refresh Preview** on the Binding Actor builds a
  temporary garment preview from a **Preview Static Mesh**, with skin weights
  and Morphs transferred and no asset created; **Delete Preview** removes it.
- Model preview from a full-character Driver: the garment surface is found
  automatically while body, face and hair stay visible, and **Driver Garment
  Slot Override** pins the garment slots when detection is ambiguous.
- Refresh reports a Ready, Warning or Error quality verdict with the measured
  reason, and refuses misaligned or unusable input.
- Cached Playback (**缓存播放**): capture the Playback Range, upload it to
  Unreal and replay every frame once at the captured rate.
- **传递 BS** in the Model workflow; turning it off gives a labelled bone-only
  comparison.
- A Real-time Preview transmission cap (Follow Scene, 30, 20 or 15 fps) while
  Maya plays.
- An **MtoU** Details section on the Binding Actor that lists the
  **Modified parts** after Refresh.
- A command-line build helper for Topia Engine 5.7.4.

### Changed

- The Maya window puts the connection row on top, with **动画**/**模型** and
  **实时预览**/**缓存播放** toggle pairs.
- Joints stream their true Maya bind pose; outfits bound at different poses
  and joints added after binding no longer reject the skeleton.
- Changing the outfit, **传递 BS**, a Preview input or the Binding, or deleting
  the actor or unloading its level, ends the connection on both hosts.
- Rendering settings are edited only on the Binding Actor's Skeletal Mesh
  Component.
- Large Morph libraries refresh faster.

### Fixed

- Unreal animation stopped updating until an unrelated edit refreshed the
  actor.
- Unreal fell visibly behind Maya when sampling flooded the connection.
- The last pose stayed applied after a disconnect.
- The Unreal listener did not recover after port 54321 was briefly in use.
- Dragging a Binding asset into a level did not create the actor.
- Placed actors did not refresh when the Binding's Driver mesh changed.
- A Driver Morph on surface missing from the Preview failed Refresh; it is now
  omitted with a warning.
- Renamed Driver material slots failed Refresh, and a failed Refresh hid the
  Driver; it now restores the Driver display. (#35)
- Outfit bones that influence only hidden meshes failed role setup with
  `BIND_POSE_INVALID`.
- The Maya window clipped its bottom when moved, and the diagnostic window did
  not wrap long text.

## 0.2.0 - 2026-08-12

### Upgrade notes

- Update the Maya and Unreal components together for protocol v2. The level
  must contain exactly one Binding Actor.

### Added

- A Chinese Maya UI with a red/green connection light, role setup, current
  outfit, scene frame rate, and bone and BlendShape diagnostics.
- The Display controller's Clothes enum selects the outfit, and only visible
  skinned meshes stream; changing the outfit disconnects.
- An action that selects every joint whose transmitted name is duplicated.
- Duplicate Maya short bone names connect when Unreal's importer gave them a
  unique numeric suffix under the same parent.

### Changed

- Maya samples at the scene's exact frame rate (1–60 fps) and follows time
  unit changes.
- Connection errors carry stable codes and detailed skeleton and BlendShape
  differences.

## 0.1.0 - 2026-08-11

First release: a Maya 2022.4 sender and a stock Unreal Editor 5.7.4 Live Link
receiver.

### Added

- Stream one evaluated deformation skeleton and its BlendShapes from Maya into
  native Live Link animation and curves, without creating an Animation
  Sequence and without moving the placed actor.
- Same-named BlendShapes on several skinned meshes merge when their values
  agree; a conflict names every differing plug.
