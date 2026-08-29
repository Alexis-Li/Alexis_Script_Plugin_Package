# Upload caches before identity-scoped local replay

Transport-paced replay from Maya could drop poses or stretch duration because a
late Maya timer delayed later submissions while Live Link Latest mode could
coalesce frames. Cached Playback therefore separates transfer from playback:
Maya captures the inclusive Playback Range, uploads the complete cache without
a real-time deadline, and waits for Unreal to validate and accept it atomically.
A partial cache is never Ready or replayable.

Unreal alone drives playback on its monotonic clock at the captured scene rate.
It applies at most one source pose per game-thread update, advances evidence
only when Live Link accepts the pose, and reports success only after every frame
was accepted exactly once in order within the duration bound. It stops with
`CACHED_PLAYBACK_PERFORMANCE` instead of skipping frames, bursting overdue
frames, or stretching a completed review.

Current protocol v6 binds `init`, each upload, each play attempt, and every
outcome to authoritative Character, `upload_id`, and `play_id` identities, so a
late reply cannot complete newer work. Encoded bytes and predicted parsed
memory are preflighted against fixed bounds; validation and runtime cache errors
discard only their attempt and keep the negotiated connection recoverable.
Returning to Real-time Preview preserves ordered controls ahead of resumed live
poses. Stop retains the last pose and cache for replay-again; clear, incompatible
revision, disconnect, or teardown releases the transient cache, and no package
or `.uasset` is created.

Protocol v5 introduced upload-then-play and remains frozen in
`conformance-v5.json`; `conformance-v6.json` is the current contract and adds
truthful identities, application evidence, resource accounting, and lossless
mode transitions without reopening the upload-before-local-replay decision.
