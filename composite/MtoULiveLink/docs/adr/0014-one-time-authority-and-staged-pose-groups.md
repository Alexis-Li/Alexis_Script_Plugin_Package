# One time authority per mode, and one staged pose group per target

Preview linkage is a third exclusive time-control state beside realtime preview
and Cached Playback. Exactly one host drives time in each mode: Maya for realtime
preview, the Unreal cache clock for Cached Playback, and the Level Sequence for
linkage. The authority is stated by both adapters, carries a `mode_revision`, and
refuses work that contradicts it, so a transition can never leave two writers
running or revive a cache that was already cleared.

Linkage keeps three positions apart instead of treating every evaluation as one
frame: `desired` is the newest camera/time target Unreal wants displayed,
`issued` is the target Maya is answering, and `applied` is the group whose
actual components were read back on the game thread. At most one target is in
flight and one newer target may wait. A continuously advancing camera replaces
only the waiting target; it never cancels the target in flight, because cancelling
it would mean that a round trip longer than one editor frame produces no complete
display at all. Pause, cut, camera-content edit and explicit discontinuity cancel
the old target, and a group that answers a superseded target is refused with
`EVAL_STALE` while the last complete display stays.

Unreal applies a staged, immutable group and reports `applied` only after the
bone, Morph and part values were read back; several Live Link publications or a
single game-thread call are not completion evidence. Both hosts therefore display
the time control owner, the target time, the latest complete application time and
the lag, and a lagging but complete group is shown as lagging rather than as
synchronisation.

Returning to realtime restores Maya's timeline, view and gate and keeps Maya
paused while Unreal keeps its takeover; only ending the preview returns the
original writers. Restores are per resource and idempotent, and a residual failure
is reported instead of being hidden behind a successful disconnect.

The scene reference follows the same ownership discipline: it lives in one
tool-owned container with an ownership mark, is imported through a staging
namespace, and is swapped, rolled back or removed only after the new content was
validated. A failed or interrupted update therefore never destroys the previous
reference, and a stale revision cannot enter linkage by renaming its version.
