# Issue #49: range capture and UE-owned cached playback

Date: 2026-09-27. Development is complete on
`codex/mtou-preview-workflow`; production-asset acceptance is pending. The
issue remains open for the owner to verify the workflow in a company scene.

## Current contract

Maya captures an inclusive integer source-frame range from its Playback Range
when the custom switch is off, or from the start/end fields when it is on.
The choice, scene rate, and source frame values are frozen at capture start;
capturing never edits the scene range. Reversed or out-of-limit ranges fail
before sampling; a single frame and a negative start are valid. Maya can
cancel, upload, show failures, reupload a compatible retained cache after
reconnect, and explicitly return to Real-time Preview.

An incomplete upload cannot become Ready. Ready holds until the UE Binding
Actor starts playback. The actor's transient view reports the source range,
captured fps, current applied source frame, count, and state. Its play, stop,
and play-again actions own local playback; stop and natural completion retain
the cache and held pose. Recapture clears the old UE attempt and waits for the
acknowledgement carrying its own `clear_id` before sampling; cancellation works
while that acknowledgement is pending. Protocol v8 reports `cache_playing` with
upload/play identity and requires a monotonically increasing `clear_id` within
each session. Details distinguishes an applied source frame −1 from no applied
pose. Both adapters reject prior protocol versions. The implementation creates
no persistent animation asset. If Maya cannot create a temporary capture cache,
it clears UE cached ownership before resuming live frames, restores the source
frame, and reports the failure in a Real-time Preview view.

## Local verification

| Check | Result |
| --- | --- |
| Maya pure Python tests | 145/145 passed, including first capture, entered, and replaying recapture cache-creation failures and partial-file cleanup |
| Maya 2024 mayapy host tests | 23/23 passed |
| Stock UE 5.7 `Build.bat UnrealEditor Win64 Development`, `-NoUBA` | Succeeded, Runtime and Editor modules |
| UE `Automation RunTests MtoULiveLink`, NullRHI | 80/80 passed: 71 without warnings, 9 with expected test or engine warnings; 0 failed/not run. Includes source-frame validity, Details text, stale clear during upload, and the cache-creation-failure control sequence restoring a displayed live pose. |
| Focused `MayaCacheReconnect` with Maya 2024 mayapy | 1/1 passed on 2026-09-26 with real host transport, actor-initiated playback, stop, replay, and custom-range recapture; not rerun for this fix |
| Protocol generator `--check`, repository validator, repository unit tests | Current v8 corpus; validator passed; 22/22 tests passed |
| Maya and UE package dry runs | Both resolved to 0.7.0 on 2026-09-26; not rerun for this fix |

The protocol v8 cross-host test used Maya 2024 mayapy with isolated preferences to create and sample a disposable
skinned scene and UE 5.7 to receive, validate, and apply it to a transient
Live Link subject. With the custom switch off, Playback Range 1–4 applied four
poses. After an actual socket loss, a new session uploaded and replayed the
same four retained poses despite an edited Maya key. A same-session recapture
with custom range −1–1 applied three new poses. UE evaluation observed root X
positions −10, 0, and 10 for source frames −1, 0, and 1. All 11 applied poses
were evaluable; the prior cache file was removed on recapture and the final
one on teardown. The test also verified that each upload waited at Ready for
the Actor action. Maya unit tests exercised the two clear-ack race paths and
cancel while waiting; UE Automation verified that a stale clear command cannot
erase an in-flight newer upload and that applied source frame −1 remains visible
in the Details summary. The cache-creation-failure regressions injected cache
factory exceptions before sampling for first capture, entered Cached Playback,
and replaying recapture. All three restored the original Maya frame, released
capture resources, queued a final clear after the last enter and before live
resume, and published a REALTIME diagnostic. UE SocketFlow independently sent
the recapture clear/enter/failure clear sequence, then observed the next live
pose in both Live Link evaluation and the displayed skeletal bone.

## Remaining acceptance

This computer has no company character or UE project assets. Repeat the
Animation Cached Playback path with a representative company character and
Binding: default and custom ranges, Ready wait, Actor play/stop/play again,
recapture, and return to real-time. Inspect the displayed character, source
frame/status text, and captured-rate behavior in the target editor. That
production scene and visual check are the remaining Issue #49 acceptance;
the local synthetic cross-host test does not substitute for them.
