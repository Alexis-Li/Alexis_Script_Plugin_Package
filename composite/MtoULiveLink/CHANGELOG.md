# Changelog

## Unreleased

- Show the complete character during Model preview without adding Binding
  inputs: the Generated Preview remains the Live Link-driven garment, while a
  second actor-owned display keeps the original full-character Driver's body,
  face, hair, and other non-garment material slots visible. The resolved
  garment slots are hidden on that Driver display to avoid overlapping the
  replacement garment, and the Driver display follows the Generated Preview's
  complete skeleton and curves. Animation preview, garment-only Drivers,
  serialized Binding fields, source assets, and protocol v6 are unchanged.
- Deepen Preview readiness on the Binding Actor (unreleased 0.4.0 C++
  interface change): Runtime callers now read one coherent
  `FMtoUPreviewReadiness` snapshot (state, build stage, ready Generated
  Preview, summary, and diagnostics) instead of six independent getters, and
  the Editor module exposes one explicit `FMtoUPreviewPreparation::RefreshActor`
  interface that returns that snapshot. Build start, monotonic stage
  observation, successful commit, failed rejection, validation-stage rejection
  of invalid ownership, invalidation, stale-result protection, and Generated
  Preview ownership are private Binding actor implementation; illegal internal
  transitions emit an `ensure` and never overwrite the current readiness, and
  a source build completion reentrant to the running refresh no longer
  self-invalidates. Connection status, display selection, the Accepted Preview
  Morph set, partial Morph coverage, bone-only comparison, and Model
  diagnostics now consume readiness without modifying it: a Preview quality
  warning remains usable readiness, and Model connection negotiation can no
  longer turn Ready into Warning. Detailed preparation results moved to an
  Editor-private header, and Runtime tests drive the private transition path
  through one development-only friend seam. Refresh remains explicit,
  synchronous, and transactional; user assets, serialized Binding fields, and
  artist-visible behavior are unchanged.
- Deepen Maya Cached Playback behind one `_CachedPlayback` action/view seam:
  Controller now attaches it eagerly to each Ready Animation Streaming session,
  forwards only user intent and terminal outcomes, and renders an immutable
  coherent view instead of inspecting cache, transport, timer, identity, or
  phase state. The module exclusively owns capture, upload, replay, retention,
  recovery, and cleanup decisions while protocol v6, cache files, UI behavior,
  diagnostics, limits, timing, and reconnect behavior remain unchanged.
- Refresh placed Binding Actors immediately when their Binding's Driver
  Skeletal Mesh changes in the asset editor.
- Add a dry-run-first Topia Engine 5.7.4 build helper that stages the project
  plugin and writable engine intermediates outside the engine and host project,
  verifies the engine/project/plugin BuildId, and installs only the five Win64
  editor binary files. Record the successful Topia compile and Athena editor
  load while keeping full functional acceptance scoped separately.
- Add a dedicated **MtoU** Details section for the Binding Actor and replace
  the Preview category's disabled property controls and duplicate diagnostics
  with one **Modified parts** row. Successful Refresh lists the unique Preview
  material slots one per line instead of exposing internal states or metrics;
  **Delete Preview** releases the generated mesh and restores the Driver display.
- Revalidate Model Preview quality and failure boundaries on the full-character
  Driver workflow: automatic garment resolution now enforces a calibrated
  source-mass boundary that refuses Refresh transactionally when the selected
  Driver surface far exceeds the Preview garment (duplicated clothing shells or
  body geometry pulled through skin-tight proximity), together with a
  twin-region rule that treats mutually coincident duplicate garment copies as
  indistinguishable sources even when nearest-ownership gives one copy every
  tie, and fails deterministically instead of guessing. Imported slot names and
  assigned materials narrow Auto's geometric candidates when they cover the
  whole Preview; incomplete or replaced material evidence falls back to pure
  geometry and never bypasses coverage validation. Resolved-garment
  normalization keeps quality metrics independent of complete-character bounds,
  and the measurement JSON reports resolved
  region and triangle counts, matched coverage, and manual flag. The external
  C01 pair `SK_C01_Clothes_09_All` plus `SM_C01_Clothes_09` measured Ready under
  the five clothing material slots with every #13 threshold retained; recorded
  evidence, hashes, rows, and boundary rationale live in the project-history
  full-character recalibration record.
- Add an advanced manual Driver garment source override for Model preview:
  when automatic resolution of the full-character Driver's garment surface is
  ambiguous, the Binding now exposes an optional **Driver Garment Slot
  Override** list that pins the source to specific Driver material slots by
  their stable imported slot names. An empty list keeps automatic resolution;
  every entry must exist exactly once on the current Driver import or Refresh
  fails with an actionable diagnostic naming the missing, duplicated, or
  stale identity, and Reimport or source edits that invalidate a persisted
  name mark readiness Dirty instead of silently returning to Auto. Manual
  selections pick whole LOD0 sections and still pass the same geometry
  coverage and alignment validation as automatic results before any weight or
  Morph transfer, changing or clearing the override releases the previous
  Generated Preview through the existing transactional lifecycle, and Refresh
  diagnostics distinguish Manual from Auto while listing the resolved slot
  identities and matched Preview coverage.
- Preview a separable garment from a full-character Driver: Model preview now
  accepts one Driver Skeletal Mesh containing body, face, hair, and one current
  outfit together with one garment-only Preview Static Mesh. Explicit Refresh
  automatically resolves the unique separable Driver garment surface from
  edge-connected regions and nearest-surface ownership of the Preview vertices,
  with imported slot names and assigned material assets as optional supporting
  evidence. Replaced Preview materials or slot-name differences fall back to
  geometry, and material agreement alone cannot authorize an unrelated region.
  Quality measurement, skin-weight transfer, and Preview Morph correspondence run only against the
  resolved surface with original Driver vertex correspondence preserved, and
  the Generated Preview keeps the complete Driver reference skeleton while
  showing only the garment. Only Driver Morphs with nonzero deltas on the
  resolved surface are generated; body, face, hair, and attachment Morphs are
  omitted with the existing warning behavior. Garment-only Drivers pass through
  the same preparation seam unchanged.
- Accept bone-only Model Preview for outfits without BlendShapes: a current
  Maya outfit that declares zero BlendShape names now negotiates Ready with an
  empty Accepted Preview Morph set even when BS transmission is enabled, with
  no placeholder Morph Target, accepted curve, or synthetic manifest entry.
  A non-empty manifest whose generated intersection stays empty is still
  refused with `PREVIEW_MORPH_MISMATCH`, partial and complete coverage keep
  their warning and Ready behavior, and disabling BS transmission on an outfit
  that declares BlendShapes remains the labelled bone-only diagnostic. Maya no
  longer shows the expression-coverage warning for the expected UE-only Morphs
  of a bone-driven outfit. Protocol v6 framing, message shapes, Cached
  Playback identity, and version negotiation are unchanged.
- Calibrate Model Preview quality warnings on the garment corpus: Refresh now
  measures the symmetric Driver/Preview surface distance (min/max/average/RMS,
  normalized by the Driver bounds) and the weight-transfer low-confidence
  ratio, then reports a Ready, Warning, or Error verdict with measured reason
  text. Misaligned inputs are rejected before mesh build, and unsafe
  low-confidence transfers are rejected transactionally during validation;
  approved production revisions measure Ready; when the public inpaint solve
  fails on large layered inputs, Refresh keeps the already-computed
  closest-point weights and warns instead of failing. Structural errors stay
  hard failures, and Morph-intersection behavior from the previous gate is
  unchanged.
- Restore the Maya Streaming session seam for Cached Playback: the streaming
  session now answers drain questions through `cached_delivery_drained()` and
  installs the cached reply listener atomically inside `pause_for_cached()`,
  so the Cached Playback module drives it only through public interface
  members instead of probing worker and phase internals. Cached-mode behavior,
  protocol v6, event kinds, and ordering are unchanged.
- Harden Cached Playback after independent review of the v6 implementation:
  Maya validates outcome evidence strictly (exact non-bool integer counts,
  finite positive elapsed durations, and an exact `ready`-revision echo of the
  negotiated `init` revision) before advancing state; manual stop now reports
  success only after Unreal's identity-matched `cache_stopped` acknowledgement;
  every terminal cached-mode path (capture cancellation, upload rejection,
  corruption, transport failure, close) shares one cleanup seam that removes
  pollers, closes upload iterators, queues the ordered `cache_clear`, and only
  then resumes live streaming; adopted caches derive their exact encoded
  declaration in bounded poller chunks instead of blocking Maya.
- Make runtime cache outcomes fully identity-scoped and recoverable on the
  Unreal side: playback-performance errors echo the owning upload/play
  identity and keep the negotiated connection open, `cache_cleared` echoes the
  dropped ownership, and a production negative-index rejection routes through
  the cache session so it atomically discards the partial upload with
  `CACHE_FRAME_INDEX_INVALID` while preserving the connection. `cache_play`
  now only initializes playback — every pose, including the first, is
  published by a later game-thread update, so one update can never apply two
  cached poses. Byte-metering rejections report the computed candidate total,
  declared size, and frozen limit.
- Treat mid-upload cache decoding/iteration failures as deterministic
  corruption: invalid UTF-8 or unreadable frame files invalidate the whole
  transient cache and resume live preview instead of masquerading as a
  transport failure that retains bad data and drops the connection.
- Restore the frozen protocol-v5 record: `conformance-v5.json` keeps its
  immutable 64 MiB boundary, and the v5 architecture history keeps its
  historical limit; the recalibrated 1 GiB / 1536 MiB bounds are described
  only under protocol v6, whose corpus boundary case moves to 1 GiB + 1 byte.
- Recalibrate the frozen transient cache limits with the first real-project
  evidence (a 320-frame, 30 fps capture whose exact encoded size exceeded the
  original estimate): encoded uploads may now use up to 1 GiB and parsed
  transient memory a fixed 1536 MiB budget, still rejected atomically beyond
  the bounds; oversized caches now report the computed size against the limit.
- Advance Cached Playback to protocol v6 (spec #17): `init` establishes the
  authoritative character snapshot revision echoed by `ready`; uploads carry
  monotonically increasing upload identities and play attempts carry play
  identities; Ready, progress, completion, stopped, cleared, and error
  outcomes echo those identities so late replies from older operations are
  ignored instead of completing newer ones. Completion now reports the exact
  accepted count, play identity, and elapsed duration.
- Make Unreal's local replay truthful: at most one cached pose is applied per
  source-frame position per update, a missed valid window stops playback with
  the stable performance error before any overdue catch-up burst, publication
  acceptance gates applied evidence, and success additionally requires total
  elapsed time within the captured-rate bound. Playback progress is bounded,
  identity-scoped, and informational only.
- Bound cache resources in production paths: actual encoded upload bytes are
  metered at the framing boundary with overflow-safe accumulation against both
  the declared size and the frozen limit, and parsed transient memory is
  preflighted from negotiated transform/curve counts against a fixed budget —
  violations reject the whole upload atomically while keeping the session open.
  Negative frame indexes now return the stable index error on the production
  socket path without closing the connection.
- Make the Real-time Preview transition lossless: queued ordered controls
  migrate into a carry-over queue when the sender returns to Latest mode, so
  cache clear stays ordered ahead of the first resumed live pose instead of
  being stranded; an explicit cache-entry control now establishes Unreal's
  cached ownership (held pose, live-frame isolation, released viewport
  realtime) before capture begins.

- Rebuild Cached Playback as upload-then-play: after capturing the inclusive
  Playback Range, Maya uploads the complete temporary cache to Unreal without a
  real-time deadline, Unreal validates and buffers it fully before replying
  Ready, and local replay is then driven by an Unreal monotonic clock at the
  captured scene rate while Maya sends only playback controls. Every cached
  frame is applied exactly once and in order; a replay that cannot sustain the
  captured rate stops with a stable playback-performance error instead of
  silently skipping or stretching, and successful completion holds the final
  pose.
- Upgrade the protocol to v5: add `cache_begin`, indexed `cache_frame`,
  `cache_end`, `cache_ready`, `cache_play`, `cache_stop`, `cache_clear`, and
  `cache_complete` messages with stable `CACHE_METADATA_INVALID`,
  `CACHE_PAYLOAD_TOO_LARGE`, `CACHE_FRAME_INDEX_INVALID`,
  `CACHE_FRAME_CONTENTS_INVALID`, `CACHE_INVALID_STATE`, `CACHE_NOT_READY`,
  `CACHE_REVISION_MISMATCH`, and `CACHED_PLAYBACK_PERFORMANCE` errors. Cache
  validation errors keep the negotiated connection open; protocol-v4 clients
  are rejected with a normal version-mismatch error. The conformance corpus
  moves to conformance-v5.json and pins every new message, limit, state
  violation, and failure boundary.
- Scope a transient Unreal cache to each negotiated streaming session: uploads
  are preflighted against frozen size and frame-count bounds, buffered frames
  are validated at the trust boundary, partial uploads can never become ready
  or replayable, ordinary live frames cannot mutate the cache or overwrite
  local playback, recapture replaces caches coherently, disconnect clears the
  buffer, and no package, `.uasset`, or Content Browser asset is ever created.
- Distinguish transfer from playback in Cached Playback statuses: capture,
  upload progress, cache-ready, Unreal-driven local replay progress, manual
  stop holding the last applied frame, replay-again reusing the uploaded
  compatible cache, explicit playback-performance failure with retry, and
  immediate live-pose submission when switching back to Real-time Preview.

- Allow Refresh Preview to complete with a warning when a localized Driver
  Morph affects only surface absent from the Preview Static Mesh. Such Morphs
  are omitted from the Generated Preview library and normal partial-coverage
  negotiation reports them instead of discarding the whole preview and hiding
  the actor.
- Restore dragging MtoU Binding assets from the Content Browser into a level
  by registering the late-loaded actor factory with both Unreal editor
  placement registries.
- Compact the Maya window layout: give the connection indicator its own top
  row above parallel **动画** and **模型** toggle buttons (selected green,
  unselected dark gray), regroup role/outfit/frame-rate texts on the left with
  bone and BlendShape counts on the right, show **实时预览** and **缓存播放**
  as a matching toggle pair, and lay out the four cached-playback transport
  buttons in a single row.
- Fix bone-only Model connections (**传递 BS** disabled): Live Link Static
  Data now publishes no property names and the ready reply reports an accepted
  morph count of zero, matching the frames that carry no PropertyValues, even
  though Maya still sends the complete curve manifest.
- Project Driver LOD0 Morphs with matching Preview surface onto the cross-topology
  Generated Preview with nearest-triangle barycentric correspondence, using
  only public stock-UE 5.7.4 APIs. Refresh remains all-or-nothing and reports
  Morph counts, sparse deltas, and projection timing. BlendShape-enabled Model
  negotiation now rejects an empty Maya/Preview intersection, warns in yellow
  for partial coverage with both difference lists, and streams only accepted
  values; full coverage is ready and non-accepted Morphs remain zero.
- Add explicit stock-UE 5.7.4 transient Skin Preview preparation. A Binding
  keeps its legacy Skeletal Mesh field as **Driver Skeletal Mesh** and adds one
  optional **Preview Static Mesh**; the actor's **Refresh Preview** command
  builds an actor-owned LOD0 Generated Preview through public Geometry APIs,
  reports five observable stages, and never creates a Content Browser asset or
  `.uasset`.
- Add transient preview lifecycle states and invalidation for Binding edits,
  PostEdit, Reimport, and source rebuilds. Repeated/failed Refresh, replacement,
  actor/world/editor teardown, and level reload release stale transient data.
  Model with **传递 BS** disabled can use the result only as a visibly labelled
  bone-only diagnostic that is not valid for model acceptance.
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
