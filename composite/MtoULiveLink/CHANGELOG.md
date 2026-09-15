# Changelog

## Unreleased

- Remove the superseded v5 conformance corpus from the source tree. Keep v6 as
  the active contract and consolidate corpus ownership and historical lookup
  guidance in the architecture record and ADR 0013.

- Add a Windows double-click Chinese wizard for the Topia build helper, with
  project/engine file pickers, a validated installation preview, and result dialogs.
  Use the built-in Windows PowerShell runtime without requiring PowerShell 7.

- Fix Issue #44 connection rejection for non-zero tiny pupil scales: UE now uses
  checked finite matrix inversion instead of an absolute determinant tolerance,
  bypasses the engine's tiny-axis identity fallback, and preserves tiny scales
  during output TRS decomposition. Zero scales remain rejected. Regression tests
  cover tiny current/bind scales, mirrored scales and tiny parents with children;
  Maya host coverage independently reconstructs bind and animated world matrices
  through multi-level non-unit TRS chains (Maya centimeters, tolerance `1e-6`).
  Add opt-in real-character Maya/UE rendered acceptance for strict remaps,
  Animation/Model poses, geometric Refresh and explicit reconnect; isolate the
  SocketFlow fixture from pending worlds left by preceding tests. See the
  complete-skeleton acceptance record for results and the retained weight warning.

- Preserve joint-to-joint structural transforms in the Maya capture (Issue #44):
  the publish traversal keeps every joint under the selected root plus the
  intermediate `transform` nodes that connect them, preserving real DAG paths
  and parent-before-child order while excluding branches without joints. Realtime
  and bind-local transforms are rebuilt over the same published parents, with
  joint-only bind queries guarded so plain transforms fall back to the existing
  capture-time re-anchoring rule. Role setup verifies that every related joint
  entered the description and reports the first missing path with its parent and
  reason (`INCOMPLETE_SKELETON`) instead of failing later with a generic
  handshake error. Strict one-to-one negotiation, namespace normalization, and
  the parent-scoped numeric-suffix rule are unchanged; the MetaHuman sample now
  publishes 1471 joints plus `joints_grp` (1472 nodes) and maps its 5 duplicate
  short names without UE or protocol changes.

- Encapsulate the Maya chunked upload in an internal transfer module (Issue #43):
  one upload pass owns the exact byte declaration, the bounded chunk pump, the
  frames-file iterator lifetime, the backpressure wait, and the post-drain
  Ready deadline behind start/advance/close in the same directly runnable
  module, returning upload progress, corruption, transport-failure, and
  payload-limit outcomes for the owner to route. Cached Playback keeps cache
  retention, upload/play ID allocation, Ready identity verification, and the
  capture/replay/detach views; protocol v6 and the upload-then-replay order
  are unchanged.

- Consolidate Unreal Cached Playback transitions in the cache-session module:
  initialize negotiated counts and revision atomically, return identity-bearing
  outcomes and realtime-override demands, and emit completion/performance failure
  once from the playback tick. The Live Link source retains transport admission,
  synchronous session publication gates, protocol encoding, and editor effects.
  Protocol v6, playback timing, and viewport behavior are unchanged.

- Complete the Issue #37 reconnect acceptance coverage with deterministic delayed old-session command/outcome delivery and an opt-in real Maya-to-Unreal retained-cache replay check. Preserve the three-connection and same-session stale-ID regression; record accepted poses and negative controls in the project history.

- Extend Preview RefreshBenchmark with deterministic 2,400/27,744-triangle garments and 13,200/147,744-triangle full-character Drivers at independently varied 4/32/64 Morphs (Issue #41). Retain the small geometry baselines, measure the public actor refresh with first-process and repeated warmed runs, verify generated output each time, and document process-memory instrumentation limits. Seconds-long waits on the larger measured corpus warrant a separate responsiveness follow-up; synchronous production behavior is unchanged.

- Validate same-surface Preview reduction across Auto's 1.70x triangle-density limit (Issue #40). Retain the conservative selection safeguards, explain that this limit can reject legitimate reduction without proving duplicate or foreign geometry, and document the verified manual garment-slot remedy. The deterministic full-character corpus covers 1.00x, 1.636x, 1.714x, and 3.00x density ratios through preparation and public Refresh readiness.
- Scope Cached Playback upload/play identities to each Streaming session (Issue #37): a newly negotiated connection accepts its own fresh upload/play sequence starting at 1, while duplicate or stale attempts within the same session stay rejected after clear and old-session commands/outcomes can never advance the new session. Protocol v6 shapes and monotonic identity semantics are unchanged.
- End the active Streaming session before explicit Preview refresh replaces its display (Issue #39): refresh requests termination through the existing idempotent boundary before releasing the mesh, and old queued frames plus cached commands fail the synchronous publish gate from that point without waiting for the worker to close the socket. Success still shows the Generated Preview immediately while disconnected and failure still keeps Error readiness with the Driver restored; Animation reconnect selects the Driver and Model reconnect renegotiates Morph coverage on the new display. Protocol v6 and the explicit refresh/reconnect boundary are unchanged.

## 0.4.0 - 2026-09-03

- Resolve renamed Driver material slots and keep the Driver visible after a failed Refresh (Issue #35): when a source group has no imported identity match, Refresh accepts one unique current displayed match corroborated by the current LOD mapping, still prefers imported identity, and still fails closed on missing, duplicate, conflicting, or shared-slot evidence without using polygon-group ordinals as slot indices; diagnostics name the affected group and distinguish missing, duplicate, and conflicting evidence with available candidates. A failed Refresh discards stale or partial preview data, keeps Error readiness with no usable preview, and restores the bound Driver display while Model connection stays blocked.

- Speed up Preview refresh Morph transfer without changing its behavior: one
  reused transient mesh and projection scratch replace the per-Morph full-mesh
  copy, so large Morph libraries refresh faster with flat peak allocation
  while geometry, skin weights, Morph coverage, diagnostics, readiness, and
  the explicit refresh boundary stay identical.
- Align the English and Chinese user documentation with the supported
  workflow: the MtoU_LiveLink Binding Actor is created by dragging a Binding
  asset into the level, the actor's Binding reference is visible but not an
  editable setup field, Model sessions with BS transmission disabled are
  documented as the labelled bone-only diagnostic excluded from model
  acceptance, and intentionally bone-driven outfits are documented as Ready
  with an empty accepted Morph set.
- Make Cached Playback recoverable, resource-bounded, and hardened against
  malformed localhost intake. Capture, upload, and cache-invalidation failures
  now complete one truthful transition: whichever already resumed Real-time
  Preview and cleared Unreal cache ownership render as Real-time with the
  failure reason, while failures that keep the negotiated cached session
  remain leaveable and retryable, and no recoverable failure leaves a
  disconnected-looking controller with disabled actions. Every Cached
  Playback state that owns the negotiated session (idle, uploading, replaying,
  completed, stopped, or cached failure) now continuously observes transport
  termination instead of waiting for the next user action, and capture checks
  transport readiness before every sample so a disconnect deletes the partial
  cache and restores the timeline immediately. Maya enforces the
  frozen cache limits incrementally: a Playback Range over 20,000 frames is
  refused before capture, and encoded frame bytes crossing 1 GiB stop the
  capture immediately and delete the partial owned cache. On the Unreal side a
  bounded producer/consumer intake queue admits parsed cache frames before
  they gain queued ownership, so a stalled Game Thread can never grow queued
  parsed memory beyond the frozen frame/byte budget; the throttle holds at
  most one raw, never-parsed frame and is released by shutdown and
  session-termination checks that keep running every worker pass, and control
  commands are never held behind the frame budget, so no disconnect, clear,
  Editor shutdown, or worker teardown can wedge; rejected,
  superseded, or cleared uploads discard all pending commands for their
  session/upload identity so they can never affect a newer attempt. Intake
  validation is message-appropriate and happens before allocation or
  conversion: a fixed 32 MiB per-message framing ceiling, streaming type
  routing without a preliminary JSON tree, negotiated cache-frame cardinality
  checks before array reservation or conversion, overflow-safe `cache_begin`
  capture-range arithmetic, and overflow-safe `int64` integer conversion. A
  recoverably rejected `cache_begin` also poisons its new upload identity, so
  pipelined frame/end messages cannot fall through to the previous upload.
  Protocol v6 message identities and connection-closing semantics
  are preserved; the conformance corpus gains the framing and overflow cases,
  and a shared `limits` block now pins the frozen framing/cache ceilings so
  each host adapter asserts its own constants against the corpus instead of
  relying on manual cross-editing.
- Keep the Streaming session and both hosts' connection state coherent:
  changing either complete Preview input (including a relevant reimport),
  deleting or releasing the Generated Preview, replacing the Binding,
  destroying the Binding Actor, or unloading (or cleaning up) the Editor
  world that owns it now routes through one idempotent session-termination
  boundary, so a new Preview revision can never receive frames negotiated
  for an older Character snapshot and neither host keeps a stale connected
  indication. The world-unload seam subscribes to the engine's synchronous
  world-cleanup broadcast, because DestroyWorld never runs Actor::Destroyed();
  unrelated editor worlds and PIE worlds can never end another session. The
  new revision still requires an explicit Refresh Preview plus a fresh
  connection. Replacing the Maya Display
  controller while connected now terminates the old session before the old
  Character scene closes, and late old-session events stay ignored. Selecting
  duplicate-bone diagnostics renders the real connection state instead of
  marking the UI disconnected while the session keeps running. Play In Editor
  is explicitly unsupported for this release: target discovery excludes PIE
  worlds, so entering PIE can never produce a multiple-Binding failure, while
  a genuine second Editor-world Binding actor still fails closed. Protocol v6
  message shapes are unchanged.
- Correct Driver garment-surface resolution and Preview quality gates: Auto
  material evidence now resolves every Driver triangle to its FINAL material
  slot through the same shared per-triangle resolver the Manual Override and
  display composition use, so reordered, unused, collapsed, or LOD-remapped
  slots can never shift the selected garment surface through a polygon-group
  or triangle-group ordinal. The minimum mass-accounting gate now uses the
  Preview TRIANGLE count consistently (a sub-floor Preview is exempt), and the
  shared resolution admission rejects zero-triangle Preview geometry, zero-area
  degenerate Driver/Preview geometry whose nonzero bounding box would otherwise
  slip past the count, coordinate-finiteness, and bounds preflight, and NaN or
  infinite Driver/Preview coordinates before any spatial indexing, coverage, or
  mass accounting can divide by zero or report a false Ready outcome. A failed
  refresh still commits no partial Generated Preview and preserves
  actor-owned readiness.
- Make Model connection and retargeting reject only genuinely unusable input:
  disabling BS transmission now negotiates as the labelled Bone-only
  comparison even when the outfit manifest has no name intersection with the
  Model display Morphs, while BS transmission with a non-empty manifest and no
  accepted Preview Morph keeps blocking with `PREVIEW_MORPH_MISMATCH`, an
  intentionally zero-name manifest stays Ready, and partial coverage keeps its
  non-blocking warning. Retargeting now validates every local source current,
  source bind, and target reference transform before publishing: a finite but
  singular matrix stops the session with a `BIND_POSE_INVALID` diagnostic that
  names the offending bone index instead of silently substituting an identity
  inversion and publishing wrong descendant transforms. Protocol v6 message
  shapes and valid bind-pose and animated-pose retargeting results are
  unchanged.
- Use the Binding Actor's `SkeletalMeshComponent` as the single editable
  rendering-settings surface. The internal full-character Driver display no
  longer duplicates every component category in Details and inherits Lighting
  Channels and Dynamic Inset Shadow when Model preview layers both meshes.
- Show the complete character during Model preview without adding Binding
  inputs: the Generated Preview remains the Live Link-driven garment, while a
  second actor-owned display keeps the original full-character Driver's body,
  face, hair, and other non-garment material slots visible. The resolved
  garment slots are hidden on that Driver display to avoid overlapping the
  replacement garment. Current LOD section metadata maps collapsed, reordered,
  and unused imported polygon groups to final Skeletal Mesh slots. A shared
  per-triangle resolver requires collapsed sources to map every triangle
  through current-LOD metadata, rejects conflicting stable metadata, and lets
  Manual Override use the same current-LOD mapping without treating
  triangle-group ordinals as slots; a triangle that maps to no final slot
  blocks the Manual override with the same fail-closed error as Auto.
  Refresh rejects any final slot shared by
  garment and visible non-garment geometry instead of guessing from polygon or
  triangle ordinals or hiding character geometry.
  The Driver display follows the Generated Preview's complete skeleton, while
  Model negotiation and frame application include Morph Targets from both
  display meshes so face, hair, and other Driver-only curves remain visible.
  Animation preview, garment-only Drivers, serialized Binding fields, source
  assets, and protocol v6 are unchanged.
- Deepen Preview readiness on the Binding Actor (0.4.0 C++
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
