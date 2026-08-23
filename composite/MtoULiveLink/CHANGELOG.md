# Changelog

## Unreleased

- Add top-level Maya **动画** and **模型** workflow buttons (startup always
  defaults to **动画**). Switching workflows disconnects, clears Animation
  cached playback, and retains the captured root, Display controller, and
  current outfit without editing the Maya scene. The Model workflow shows the
  role, Display/outfit, frame rate, transmission-cap, connection-state, and
  diagnostic controls plus a default-on **传递 BS** toggle, and hides the
  cached-playback controls.
- Upgrade the protocol to v4: `init` requires `workflow` (`animation` or
  `model`) and `blendshapes_enabled`, `ready` echoes the workflow and reports
  `target_morph_count` and `accepted_morph_count`, and protocol-v3 clients are
  rejected with a normal `PROTOCOL_VERSION_MISMATCH` response. Add the stable
  Model-preview error codes `PREVIEW_NOT_READY`, `PREVIEW_BUILD_FAILED`, and
  `PREVIEW_MORPH_MISMATCH`. Both workflows keep one subject, one localhost
  port, and one session; Animation continues to drive the Driver Skeletal
  Mesh while Model connections refuse until a ready Generated Preview exists.
- Changing the Clothes enum or the **传递 BS** toggle while connected now
  disconnects the session and requires a fresh negotiation.

- Add Cached Playback with mutually exclusive **实时预览** and **缓存播放** Maya
  modes. Ready connections can capture the inclusive Playback Range into an
  incrementally written, atomically finalized system-temporary cache, restore
  Maya's original current frame, and replay every protocol-v3 frame once in
  order at the captured scene rate. Completed caches can be replayed again,
  remain temporary, and are cleaned up on replacement, cancellation, scene or
  character changes, tool close, Maya exit, and qualifying stale startup cleanup.
- Harden Cached Playback capture cleanup: progress-time cancellation cannot
  publish a partial final frame, failed timer or disk-space checks restore Maya
  state and close temporary files, recapture stops active replay first, and
  completed metadata is restricted to the exact owned frame range and paths.
- Harden Cached Playback lifecycle: manual stop holds the last sent frame and
  reopens ordered transport for repeat replay, transport failures retain a
  completed cache for reconnect, incompatible failures and disconnects remove
  owned cache files, and stale session callbacks cannot affect a newer UI
  session. Add an explicit **停止回放** control and cached-state status.
- Retry the Unreal listener when another local process temporarily owns port
  54321, so it starts listening automatically after the port is released.
- Add a remembered Maya Real-time Preview playback transmission cap with
  Follow Scene, 30 fps, 20 fps, and 15 fps choices. Numeric caps apply only
  while Maya is playing; paused posing and scrubbing stay at the scene rate,
  playback stops submit the final pose, and changing the cap keeps the current
  streaming session and character snapshot.
- Upgrade the protocol to v3 and transmit each joint's true Maya bind-local
  transform from SkinCluster `bindPreMatrix` and bind-pose `dagPose` data.
- Follow the joint `bindPose` attribute's downstream connection when reading
  bind-pose `dagPose` matrices, so outfit bones that only influence hidden
  meshes no longer fail role setup with `BIND_POSE_INVALID`.
- Fall back to the character-setup-time pose for joints without any saved
  bind data (for example corrective slider joints added after binding),
  anchored to the parent's bind frame, instead of rejecting the skeleton.
- Map evaluated source component-space motion from the Maya bind pose onto the
  Unreal Skeletal Mesh reference pose, including differing local bone axes,
  parent-child motion, translation, and unit scale.
- Resolve bind-matrix conflicts between skin clusters (outfits bound at
  different poses) from the joint-connected bind-pose `dagPose` — the pose Go
  to Bind Pose restores and typically the Skeletal Mesh export pose — falling
  back to the skin cluster with the most influences, and report the resolved
  conflict count after every character capture instead of rejecting the
  skeleton. Equally ranked candidates must agree, so a node name cannot choose
  the bind pose. Non-finite or non-invertible matrices are rejected before
  inversion.
- Update Live Link animation continuously in Unreal Editor instead of waiting
  for an unrelated property edit to refresh the binding actor, temporarily
  forcing level viewports into realtime mode only while a stream is connected.
- Cap Maya sampling to the scene frame rate when timer and time-change events
  arrive together, preventing large character frames from flooding the TCP
  stream and leaving Unreal visibly behind Maya.
- Clear the last streamed Live Link frame when a Maya session disconnects so a
  stale or malformed pose cannot remain applied across reconnection attempts.
- Define the protocol v3 wire fields, stable parser error categories, and
  connection-closing policy in one canonical cross-host conformance corpus.
- Exercise applicable canonical cases through both Maya and Unreal adapters,
  and fail repository validation when generated Unreal test data is stale.
- Isolate Maya character capture, scene revision, outfit refresh, and pose
  sampling behind one character-scene interface.
- Isolate each Maya streaming attempt behind a transactional session lifecycle
  with frame-rate changes, first-terminal-outcome handling, and idempotent
  cleanup of its worker, timer, and callbacks.
- Keep the Maya sender window fitted to its controls so moving the window no
  longer alternates between a clipped and fully visible bottom section.
- Simplify the Maya diagnostic-details window to error-specific information,
  wrap long text, and keep its text area fitted to the resizable window.
- Consolidate the bilingual README, changelog, license, and project rules at
  the composite root while keeping host tests beside their component code.
- Keep packaged components limited to their host runtime files.

## 0.2.0 - 2026-08-12

- Add a Chinese Maya UI with a red/green connection light, role setup,
  current-outfit, scene-frame-rate, streamed-bone, and BlendShape diagnostics.
- Detect the character Display controller and its Clothes enum, stream only
  effectively visible skinned meshes, and disconnect when the outfit changes.
- Sample at the Maya scene's exact 1-60 fps rate and rebuild the timer when the
  time unit changes.
- Upgrade the local protocol to version 2 with stable error codes, detailed
  skeleton diagnostics, bidirectional BlendShape differences, and an exactly
  one Binding Actor requirement.
- Add a Maya action that selects all joints involved in duplicate transmitted
  bone names by full DAG path for quick Outliner inspection.
- Allow duplicate Maya short bone names when Unreal can uniquely map its
  importer-added numeric suffixes within the corresponding parent branch.

## 0.1.0 - 2026-08-11

- Provide the Maya 2022.4 sender and stock Unreal Editor 5.7.4 Live Link
  receiver as the first production-accepted MtoU_LiveLink release.
- Stream one evaluated deformation skeleton and matching BlendShapes from Maya.
- Merge same-named BlendShapes across skinned mesh parts when their evaluated
  values agree and report every conflicting plug when they differ.
- Receive native Live Link animation and curves while preserving placed actor
  transforms and without creating an Animation Sequence.
