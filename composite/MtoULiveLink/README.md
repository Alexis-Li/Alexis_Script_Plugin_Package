# MtoU_LiveLink

[中文说明](README_CN.md)

## Introduction

MtoU_LiveLink is a composite Maya and Unreal plugin for locally previewing one
evaluated Maya deformation skeleton and its matching BlendShapes in Unreal
Live Link. Its two components communicate over a local loopback connection
while remaining independently installable and packageable.

## Supported Versions

- Windows 64-bit
- Autodesk Maya 2022.4
- Stock Unreal Editor 5.7.4

Compatibility with third-party Unreal Engine 5.7 builds is not claimed.

## Installation

Install the matching component in each host:

1. Maya: copy `maya/MtoULiveLink/scripts/MtoULiveLink.py` to a Maya scripts
   directory, or run it directly in Maya's Python Script Editor.
2. Unreal: copy the complete `unreal/MtoULiveLink/` directory to
   `<Project>/Plugins/MtoULiveLink/`. The installed descriptor must be
   `<Project>/Plugins/MtoULiveLink/MtoULiveLink.uplugin`.
3. Compile the Unreal project, enable **Live Link** and **MtoU_LiveLink**, and
   restart Unreal Editor.

## Usage

1. In Unreal, create an **MtoU_LiveLink Binding**, assign its **Driver Skeletal
   Mesh**, and optionally assign the garment **Preview Static Mesh** directly
   below it. Keep exactly one binding actor in the level and assign the Binding
   to that actor. For Model preview, select the actor and use **Refresh Preview**
   explicitly. Refresh reads LOD0 source data and creates only actor-owned
   transient data, including one projected Morph Target for every Driver Morph;
   input edits or source rebuilds mark it Dirty and require another explicit
   Refresh. The Refresh is transactional: an invalid required Morph leaves no
   partial Generated Preview active.
2. In Maya, run `MtoULiveLink.py`, select exactly one deformation root, and
   select **Set Character**. The tool finds the character's `Display_ctrl` and
   Clothes enum; use the manual Display button if discovery is ambiguous.
   If duplicate transmitted bone names are found, use **Select Duplicate
   Bones** in the error dialog or main window to select every conflicting joint
   by its full DAG path and locate it in the Outliner.
   Duplicates do not fail immediately: Unreal maps a uniquely numeric-suffixed
   imported bone below the already matched parent, warns on success, and rejects
   an ambiguous mapping.
3. Confirm the displayed outfit and scene rate, choose a playback transmission
   cap (**Follow Scene**, **30 fps**, **20 fps**, or **15 fps**), then select
   **Connect**. The default is **20 fps** and the choice is remembered in Maya
   native option storage.
4. Pick the top-level workflow with the **动画** and **模型** buttons; startup
   always defaults to **动画**. Switching workflows disconnects the current
   session and clears Animation cached playback while retaining the captured
   root, Display controller, and current outfit; the Maya scene is never
   edited. The **模型** workflow shows the same role, Display/outfit, frame
   rate, transmission-cap, connection-state, and diagnostic controls plus a
   default-on **传递 BS** toggle, and hides the cached-playback controls.
   Connecting in **模型** requires a ready Generated Preview Skeletal Mesh on
   the Unreal binding actor; otherwise the connection is refused with
   `PREVIEW_NOT_READY` and the visible target is unchanged. With **传递 BS**
   disabled, Model can connect to that preview as a visibly labelled bone-only
   diagnostic that is not valid for model acceptance. With **传递 BS** enabled,
   Unreal accepts only the intersection of the current Maya outfit's
   BlendShapes and the Generated Preview Morph library. An empty intersection
   is refused with `PREVIEW_MORPH_MISMATCH`; partial coverage connects with a
   yellow warning and both difference lists, while full coverage connects as
   ready. Only accepted values are streamed, so UE-only Morphs remain at zero.
5. Pose, play, or scrub in Maya. The cap applies only during Maya playback;
   paused posing and manual timeline changes continue at the scene rate, and
   stopping playback submits the final pose immediately. Changing the Clothes
   enum or the **传递 BS** toggle while connected disconnects the session;
   replace the Unreal binding actor after an outfit change and reconnect for a
   fresh negotiation.
6. In the **动画** workflow, use the mutually exclusive **实时预览** and
   **缓存播放** mode controls for review. Cached Playback requires a ready
   connection, pauses live sampling, and disables the real-time cap.
   **捕获并回放** samples the current Maya Playback Range inclusively, writes
   protocol-v4 frames incrementally to the user's system temporary directory,
   restores the original current frame, and replays every captured frame once
   at the recorded scene rate. Capture shows current/total progress, stops
   Maya playback before sampling, and can be canceled. The tool estimates
   temporary-disk usage before writing frames, asks for confirmation above
   1 GiB, and rejects a range when free space is insufficient; failed or
   canceled capture removes its partial cache.
7. While replaying, the status explicitly says that Unreal is showing the
   captured cache rather than the current Maya pose. Use **停止回放** to hold
   the last frame sent and retain the cache, or **再次回放** to replay it
   without recapturing. Switch back to **实时预览** to stop replay and
   immediately submit Maya's current pose. Cached Playback creates no Unreal
   asset; the completed cache is kept only for the compatible Maya session and
   is removed when replaced, canceled, disconnected, the scene or character
   changes, the tool closes, or Maya exits. Startup removes only valid
   MtoU-owned cache remnants older than 24 hours.

**View Diagnostic Details** shows only the current error summary, solution,
code, and relevant technical details. Long details wrap to the window width;
character and scene values already visible in the main window are not repeated.

The receiver preserves each placed actor transform and does not create an
Animation Sequence. The binding actor updates its animation continuously in
Unreal Editor. While connected, the plugin temporarily forces level viewports
into realtime mode and restores each viewport's prior setting on disconnect.
Disconnecting clears the last streamed frame so the mesh returns to its
reference pose instead of retaining a stale pose. Maya's saved SkinCluster bind
pose is kept separate from the current animation frame and mapped to the target
Skeletal Mesh reference pose, so connecting does not require frame 1 or the
current frame to be an A Pose. Joints without saved bind data, such as corrective slider joints added
after binding, use their pose at character setup time. Bind matrices that
disagree across skin clusters, which happens when outfits were bound at
different poses, are resolved from the bind-pose `dagPose` (the pose Go to Bind
Pose restores) or the skin cluster with the most influences, and the resolved
conflict count is shown after every character capture. If equally ranked
candidates disagree, role setup stops instead of choosing a pose from the skin
cluster node name. Non-finite or non-invertible bind matrices are rejected
before inversion. Same-named BlendShapes
on separate skinned mesh parts are sent as one Unreal curve when their evaluated
values agree. If those values differ, sampling stops and identifies every
conflicting Maya plug. Maya- and Unreal-only BlendShape names are non-blocking
warnings in the Animation workflow. Protocol version 4 requires matching Maya
and Unreal components installed together: `init` carries the selected
**动画**/**模型** workflow and the **传递 BS** choice, and `ready` echoes the
workflow with target and accepted Morph counts. Protocol-v3 clients are
rejected with a normal version-mismatch error. The Model workflow additionally
uses the stable `PREVIEW_NOT_READY`, `PREVIEW_BUILD_FAILED`, and
`PREVIEW_MORPH_MISMATCH` errors. After Unreal Refresh, BlendShape-enabled Model
preview streams the non-empty Maya/Generated Preview intersection; partial
coverage is visibly yellow and full coverage is ready. The bone-only path
remains visibly labelled and is not valid for model acceptance.
