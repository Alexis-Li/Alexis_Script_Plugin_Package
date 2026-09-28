# Issue #50: interactive cached playback (pause, seek, loop)

Accepted on 2026-09-28 on `codex/mtou-preview-workflow`, based on
`29beaa88bd2907fb1c5b7e79dd3b490095a94c22`. Issue #49 (range capture, upload, and
UE-owned first playback) is closed, which was this issue's only blocker.

## Current contract

After a validated upload, the Unreal Binding Actor owns every playback action:
播放／再次播放, 暂停, 继续, 停止, a Maya source-frame field with 定位, and the
循环 selection. Playback is organized in **segments**. Starting an attempt or
seeking opens a segment, resuming shifts its timing baseline past the paused
interval, and a loop round continues the same schedule instead of restarting it.

- Pause holds the pose, the cache, and the schedule. No pose becomes due while
  held, so an arbitrarily long pause can never produce
  `CACHED_PLAYBACK_PERFORMANCE`; resume continues from the held position and
  never replays the paused interval.
- Seek accepts only a Maya source frame the cache actually sampled, displays
  it, and holds it paused. An out-of-range frame is refused with
  `CACHE_SEEK_INVALID` naming the requested frame and the sampled range; it is
  neither clamped nor reported to Maya, because the action exists only in
  Unreal.
- Loop wraps from the accepted last frame to the first frame on the same
  continuous schedule, so rounds neither burst nor double-complete. Turning it
  off completes the current round, and a one-frame cache advances one round per
  captured interval instead of busy-looping.
- Stop ends a running or a held attempt and retains the cache and the last
  displayed pose.
- Completion evidence is scoped: `cache_complete` reports `scope: "cache"` only
  for one unbroken whole-cache run, and `scope: "segment"` with its own
  `start_frame`, `end_frame`, and `applied_frame_count` after a seek or a
  completed loop round. Paused time is excluded from the measured duration
  rather than counted as lateness.

Protocol v9 adds `cache_paused`, `cache_resumed`, `cache_seeked`, `cache_looped`,
and `cache_loop_changed`, and extends `cache_playing` (loop selection) and
`cache_complete` (scope and source range). Maya renders pause, seek, loop round,
stop, completion, and segment completion from those outcomes without a
re-upload. Recapture, clear, disconnect, and character or Preview input changes
drop the attempt's loop selection, positioned pose, and segment state, and stale
`play_id` outcomes are ignored. Both components identify as 0.9.0. The decision
itself is recorded in [ADR 0013](../../../composite/MtoULiveLink/docs/adr/0013-upload-the-cache-before-local-replay.md).

## Acceptance evidence

Maya 2024 (API 20240200) and UE 5.7.4 (CL 51494982) were used. The peer-driven
checks ran the production Maya controller, capture, upload, cache files, and
transport from `mayapy`, and drove Unreal through the public actor operations.

| Independently executed check | Result |
| --- | --- |
| Maya pure Python `test_mtou_livelink.py` | 158/158 passed |
| Maya 2024 `maya_host_tests.py` | 23/23 passed |
| UE `Build.bat UnrealEditor Win64 Development -NoUBA` | Succeeded |
| UE `Automation RunTests MtoULiveLink`, NullRHI | 83/83 passed, 0 failed or unrun |
| `MtoULiveLink.CachedPlayback.Segments` (deterministic clock) | Passed; pause/resume with a 120s held interval, seek refusals and positions, two loop rounds, single-frame looping, stop from pause, and recapture isolation |
| `MtoULiveLink.Editor.DetailsInteractiveStateText` | Passed; pause, seek, loop round, stop, completion, and failure stay distinguishable |
| `MtoULiveLink.Protocol.ConformanceCorpus` | Passed over the v9 corpus, including every new outcome and rejected payload |
| `MtoULiveLink.Source.MayaInteractivePlayback` | Passed through the real mayapy peer and its wire outcomes |
| `MtoULiveLink.Source.MayaCacheReconnect` | Passed; reconnect, retained-cache upload, and stale-identity isolation with the v9 shape |
| Protocol corpus generator `--check`, repository validator, repository unit tests | Current v9 corpus; validator passed; 22/22 tests passed |

## Interactive playback through the real transport

One 24-frame capture at the scene rate (frames 1–24, `translateX = 10 × frame`)
was uploaded once and then driven entirely from the Unreal Binding Actor while
the Maya peer recorded what it received and rendered.

- Playback applied frames 1…24 in order. Pause was accepted at source frame 2
  with 2 applied poses; the pose stayed fixed for a 2.5s hold with no
  performance failure, and resume applied frame 3 next, so no paused interval
  was replayed and no pose was skipped. Maya displayed
  缓存已暂停于源帧 2（已应用 2/24 帧）.
- Seeking forward to 20 and backward to 5 each displayed its target and held it
  (Maya: 已定位到源帧 20／5，缓存已暂停). A request for source frame 99 was refused
  with `CACHE_SEEK_INVALID` naming `(1..24)`, left the held frame at 5
  unchanged, and produced no wire outcome.
- With 循环 enabled, the attempt wrapped after the accepted last frame twice:
  Maya received `cache_looped round 1` and `round 2`, and the applied pose
  sequence was exactly 1…24, 1…24 with one pose per captured interval. Turning
  loop off completed the current round as `cache_complete scope segment`
  (applied 24, source 1–24, elapsed 0.768s for a 0.8s schedule) and Maya
  displayed 播放段完成（源帧 1–24），已停在最后一帧.
- Both endpoints stayed addressable afterwards (seeks to source frame 1 and 24
  displayed 10 and 240), and 停止 held the retained cache.
- Leaving cached playback cleared the Unreal cache and resumed live poses while
  the completed local cache stayed for a later upload. A real transport loss
  followed by a reconnect renegotiated, captured, and uploaded a fresh cache;
  that reconnected attempt played to completion as `cache_complete scope cache`
  (applied 24, elapsed 0.767s), proving the whole-cache claim still holds after
  a reconnect. Closing the controller deleted both transient caches, and the
  reconnect leg reported no diagnostic.
- The peer's outcome record contained no `error` outcome, so the refused local
  seek never reached Maya, and its view snapshots showed PAUSED, SEEKED, and
  REPLAYING-with-second-round states matching the same source frames.

## Limits

- Verification used disposable synthetic Maya characters over the real
  transport, not a company production scene; the controller, capture, upload,
  cache files, commands, and pose values are the production ones.
- The Unreal controls were exercised through the public actor API and the
  exported Details text functions; the Details row itself was not clicked by a
  human, and no viewport screenshots were taken for this issue.
- Cached playback remains the only time owner in this workflow. Hosting it under
  an external time driver such as Sequencer is the separate Issue #52
  validation and is not established here.
