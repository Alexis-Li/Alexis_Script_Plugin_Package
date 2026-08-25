# Upload the cache before Unreal plays it locally

MtoU_LiveLink Cached Playback previously replayed a completed Maya cache by
reading one cached frame per Maya timer tick and sending it over TCP at the
scene-frame interval. A late Maya callback delayed every later callback, and
ordered submission from Maya proved nothing about what Unreal actually applied,
because Live Link Latest mode and one pending-frame slot coalesce frames. Under
load the review either dropped poses or stretched duration — both unacceptable
for final-animation review. This decision supersedes the ordered
transport-paced replay of specification #3 (including its "allow duration to
increase" fallback) while keeping lifecycle ticket #7's retention behavior.

Cached Playback now separates cache transfer from cache playback. Maya still
captures every inclusive Playback Range display frame into its temporary disk
cache. Before any playback, Maya uploads the complete cache without a
real-time deadline: `cache_begin` declares revision, captured range, scene
rate, frame count, and encoded size; indexed `cache_frame` messages follow in
order; `cache_end` finishes the upload. Unreal validates metadata, frame
count, contiguous zero-based indices, transform/curve contents against the
negotiated counts, and frozen resource bounds, buffers everything transiently,
and only then replies `cache_ready`. A partial cache is never Ready and never
replayable.

After Ready, Unreal drives local playback on its own monotonic clock using the
captured scene rate. `cache_play` carries the expected snapshot revision so a
stale request cannot replay an incompatible buffer. Unreal applies each cached
frame exactly once, in order, through Live Link publication, holds the final
pose on success (`cache_complete` with applied-frame evidence), and stops with
a stable `CACHED_PLAYBACK_PERFORMANCE` error when it cannot sustain the rate —
it never silently skips a frame or stretches a completed review. `cache_stop`
holds the last applied frame and retains the cache for replay-again;
`cache_clear` drops it when returning to Real-time Preview.

This requires protocol v5. The new messages and stable errors
(`CACHE_METADATA_INVALID`, `CACHE_PAYLOAD_TOO_LARGE`,
`CACHE_FRAME_INDEX_INVALID`, `CACHE_FRAME_CONTENTS_INVALID`,
`CACHE_INVALID_STATE`, `CACHE_NOT_READY`, `CACHE_REVISION_MISMATCH`,
`CACHED_PLAYBACK_PERFORMANCE`) are frozen in conformance-v5.json, which also
pins state violations against a seeded cache session and rejects protocol v4
with `PROTOCOL_VERSION_MISMATCH`. Cache validation errors keep the negotiated
connection open so retrying never requires renegotiating skeleton or Morphs.
The transient Unreal cache is scoped to one negotiated session: uploads are
preflighted before allocation, ordinary live frames cannot mutate it or enter
local playback, recapture replaces it coherently, disconnect clears it, and no
package or `.uasset` is ever created.
