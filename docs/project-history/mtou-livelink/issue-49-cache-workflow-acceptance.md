# Issue #49: range capture and UE-owned cached playback

Date: 2026-09-26. Development is complete on
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
the cache and held pose. Recapture clears the old UE attempt and waits for its
identity-matched acknowledgement before sampling. Protocol v7 reports
`cache_playing` with upload/play identity, and both adapters reject prior
versions. The implementation creates no persistent animation asset.

## Local verification

| Check | Result |
| --- | --- |
| Maya pure Python tests | 138/138 passed |
| Maya 2024 mayapy host tests | 23/23 passed |
| Stock UE 5.7 `Build.bat UnrealEditor Win64 Development`, `-NoUBA` | Succeeded, Runtime and Editor modules |
| UE `Automation RunTests MtoULiveLink`, NullRHI | 78/78 passed: 69 without warnings, 9 with expected test or engine warnings; 0 failed/not run |
| Focused `CacheSession`, `SocketFlow`, `MayaCacheReconnect` with Maya 2024 mayapy | 3/3 passed, including actor-initiated playback, stop, replay, single-frame completion, and real host transport |
| Protocol generator `--check`, repository validator, repository unit tests | Current corpus; validator passed; 22/22 tests passed |
| Maya and UE package dry runs | Both resolve to 0.6.0; no archives written |

The cross-host test used Maya 2024 mayapy with isolated preferences to create and sample a disposable
skinned scene and UE 5.7 to receive, validate, and apply it to a transient
Live Link subject. With the custom switch off, Playback Range 1–4 applied four
poses. After an actual socket loss, a new session uploaded and replayed the
same four retained poses despite an edited Maya key. A same-session recapture
with custom range −1–1 applied three new poses. UE evaluation observed root X
positions −10, 0, and 10 for source frames −1, 0, and 1. All 11 applied poses
were evaluable; the prior cache file was removed on recapture and the final
one on teardown. The test also verified that each upload waited at Ready for
the Actor action.

## Remaining acceptance

This computer has no company character or UE project assets. Repeat the
Animation Cached Playback path with a representative company character and
Binding: default and custom ranges, Ready wait, Actor play/stop/play again,
recapture, and return to real-time. Inspect the displayed character, source
frame/status text, and captured-rate behavior in the target editor. That
production scene and visual check are the remaining Issue #49 acceptance;
the local synthetic cross-host test does not substitute for them.
