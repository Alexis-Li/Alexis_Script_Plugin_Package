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

Ready waits for an explicit Unreal Binding Actor action; Maya has no play,
pause, seek, loop, or stop action. In Cached Playback, Unreal alone drives
playback on its monotonic clock at the captured scene rate. Local playback is
organized in **playback segments**: one ordered run of buffered poses with its
own timing baseline. Starting an attempt or seeking opens a segment, resuming
rebuilds the segment's baseline past the paused interval, and a loop round
continues the same schedule instead of restarting it.

- It applies at most one source pose per game-thread update, advances evidence
  only when Live Link accepts the pose, and never bursts overdue frames.
- **Pause** holds the current pose, the cache, and the schedule. No pose is
  due while held, so an arbitrarily long pause can never produce
  `CACHED_PLAYBACK_PERFORMANCE`. **Resume** shifts the segment's baseline by
  the paused interval: playback continues from the held position, the paused
  interval is never replayed, and the remaining interval of the current slot
  is preserved.
- **Seek** accepts only a Maya source frame the current cache actually
  sampled. It displays that frame immediately and enters the paused state,
  which opens a new segment at the target; an out-of-range frame is refused
  with `CACHE_SEEK_INVALID` naming the requested frame and the sampled range
  instead of being silently clamped.
- **Loop** wraps from an accepted last frame to the first frame and keeps the
  continuous schedule, so repeated rounds never busy-loop or burst. Turning
  loop off completes the current round.
- Unreal stops with `CACHED_PLAYBACK_PERFORMANCE` instead of skipping frames,
  bursting overdue frames, or stretching a completed review.

Completion evidence is scoped. `cache_complete` carries `scope: "cache"` only
when one unbroken segment applied every buffered pose exactly once, in order,
from the first source frame within the duration bound. A seek or a completed
loop round ends the attempt's whole-cache claim: such a segment completes with
`scope: "segment"` and reports its own `start_frame`, `end_frame`, and
`applied_frame_count`. Paused time is excluded from the measured duration
rather than counted as lateness. Complete upload, bounded resources,
stale-command isolation, and no silent frame dropping during sequential
playback remain required.

This clock ownership is scoped to Cached Playback, not all future preview modes.
The [Issue #52 prototype](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/issues/52)
may disable cached playback while validating explicit Sequencer time ownership
and restoration. Production integration requires a decision on mutually
exclusive time control; it is not established by this cache decision.

Current protocol v9 binds `init`, each upload, each UE play attempt, each
pause, resume, seek, loop selection, and each clear request to authoritative
Character, `upload_id`, `play_id`, and `clear_id` identities. Unreal reports
`cache_playing` (with the attempt's loop selection) before that attempt's
progress, pause, seek, loop, completion, or stop; every later outcome carries
the same `play_id`, and Maya ignores older identities. `cache_clear` carries a
positive session-scoped `clear_id`; Unreal echoes it in `cache_cleared` and
rejects reused clear IDs. Maya waits for that exact acknowledgement even when
an earlier play notification has not yet reached it. New uploads reset only
the active play identity, and a recapture, clear, or disconnect also drops the
attempt's loop selection, positioned pose, and segment state; the session's
increasing play-ID history remains. Encoded bytes and predicted parsed
memory are preflighted against fixed bounds; validation and runtime cache errors
discard only their attempt and keep the negotiated connection recoverable.
Preparing a new capture clears and ends the old UE cache first; Maya waits for
the matching `cache_cleared` acknowledgement before sampling and can cancel
while waiting. Returning to
Real-time Preview preserves ordered controls ahead of resumed live poses. Stop
retains the last pose and cache for replay-again; clear, incompatible
revision, disconnect, or teardown releases the transient cache, and no package
or `.uasset` is created.

Upload/play/clear identity history belongs to one Streaming session: clearing a cache
preserves that session's stale-ID rejection, while a new negotiated session
starts fresh and accepts its own sequence from 1. Delayed commands and outcomes
from an older session cannot advance the new one. See the
[reconnect acceptance](../../../../docs/project-history/mtou-livelink/cached-playback-reconnect-acceptance.md)
for the deterministic and real-host evidence.

Both adapters require protocol v9. Its executable contract is
[`conformance-v9.json`](../../protocol/conformance-v9.json), covering message
identities, application evidence, playback segments and their completion
scopes, pause/seek/loop outcomes, resource accounting, and mode transitions.
The source tree retains protocol corpora only for versions with active
consumers and corresponding tests. Superseded contracts are preserved in Git;
their historical limits do not govern the current adapters.
