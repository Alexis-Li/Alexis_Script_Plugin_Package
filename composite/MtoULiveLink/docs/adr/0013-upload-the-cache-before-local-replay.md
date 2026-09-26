# Upload caches before identity-scoped local replay

Transport-paced replay from Maya could drop poses or stretch duration because a
late Maya timer delayed later submissions while Live Link Latest mode could
coalesce frames. Cached Playback therefore separates transfer from playback:
Maya captures an inclusive integer range from either its current Playback Range
or the enabled custom start/end fields. It freezes the range and scene rate at
capture start without editing the scene range, uploads the complete cache
without a real-time deadline, and waits for Unreal to validate it atomically.
A partial cache is never Ready or replayable.

The Playback Range endpoints use Maya's existing nearest-integer sampling
conversion (half-integer ties round to even); custom fields already contain
integers. A reversed range fails before sampling. One-frame and negative-start
ranges are valid, subject to the same 20,000-frame and 1 GiB limits. A
one-frame playback may complete with zero measured elapsed time.

Ready waits for an explicit Unreal Binding Actor action; Maya has no play or
stop action. Unreal alone drives playback on its monotonic clock at the captured
scene rate. It applies at most one source pose per game-thread update, advances evidence
only when Live Link accepts the pose, and reports success only after every frame
was accepted exactly once in order within the duration bound. It stops with
`CACHED_PLAYBACK_PERFORMANCE` instead of skipping frames, bursting overdue
frames, or stretching a completed review.

Current protocol v7 binds `init`, each upload, each UE play attempt, and every
outcome to authoritative Character, `upload_id`, and `play_id` identities. Unreal
reports `cache_playing` before that attempt's progress, completion, or stop;
Maya ignores older identities. Encoded bytes and predicted parsed
memory are preflighted against fixed bounds; validation and runtime cache errors
discard only their attempt and keep the negotiated connection recoverable.
Preparing a new capture clears and ends the old UE cache first; Maya waits for
the matching `cache_cleared` acknowledgement before sampling. Returning to
Real-time Preview preserves ordered controls ahead of resumed live poses. Stop
retains the last pose and cache for replay-again; clear, incompatible
revision, disconnect, or teardown releases the transient cache, and no package
or `.uasset` is created.

Upload/play identity history belongs to one Streaming session: clearing a cache
preserves that session's stale-ID rejection, while a new negotiated session
starts fresh and accepts its own sequence from 1. Delayed commands and outcomes
from an older session cannot advance the new one. See the
[reconnect acceptance](../../../../docs/project-history/mtou-livelink/cached-playback-reconnect-acceptance.md)
for the deterministic and real-host evidence.

Both adapters require protocol v7. Its executable contract is
[`conformance-v7.json`](../../protocol/conformance-v7.json), covering message
identities, application evidence, resource accounting, and mode transitions.
The source tree retains protocol corpora only for versions with active
consumers and corresponding tests. Superseded contracts are preserved in Git;
their historical limits do not govern the current adapters.
