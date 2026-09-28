# Issue #49: range capture and UE-owned cached playback

Accepted on 2026-09-28 against `29beaa88bd2907fb1c5b7e79dd3b490095a94c22`,
which includes the reviewed fixes through `f741bfd922409b8acd768b17191b1af29ca88325`.
The earlier four review findings are resolved. Representative C01 asset
acceptance is complete; no remaining blocking finding was identified.

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

## Acceptance evidence

Maya 2024 (API 20240200) and UE 5.7.4 (CL 51494982) were used with the supplied
C01 `111_MH_Backups.0002.ma` scene and assets from the Backups project. The
transient Binding used `SK_C01_CombineBody_Clothes_12` as its primary mesh and
`SK_C01_Head` / `SK_C01_Hair_01` as additional parts. The scene, Binding and
project assets were not saved or modified. The installed project's plugin
sources differed from HEAD, so acceptance loaded the current source/build in
a disposable host with the project's assets and KawaiiPhysics dependency.

| Independently executed check | Result |
| --- | --- |
| Maya pure Python `test_mtou_livelink.py` | 145/145 passed |
| Maya 2024 `maya_host_tests.py`, isolated preferences | 23/23 passed |
| UE `Build.bat UnrealEditor Win64 Development -NoUBA` | Succeeded |
| Unmodified HEAD `Automation RunTests MtoULiveLink`, NullRHI | 80/80 passed: 71 without warnings, 9 with warnings; no failures or unrun tests |
| `MtoULiveLink.Source.MayaCacheReconnect`, actual mayapy transport | Passed; all 11 cached poses evaluated across capture, socket loss, retained upload and custom recapture |
| `MtoULiveLink.Source.MayaCharacterPartsHost`, C01 | Passed with asset-version warnings; 872 required bones and owning-mesh morph evaluation checked during live, cached, replayed, stopped and returned-live states |
| Rendered C01 acceptance, scratch extension of `CharacterAcceptance` | Passed with asset-version warnings; default/custom capture, Ready wait, play/replay/stop, recapture and return to live |
| Protocol corpus generator `--check` | Passed |

### Production workflow and frame evidence

- Custom range off: the disposable Maya Playback Range was 1–24 at 30 fps.
  After upload, UE remained Ready for an explicit one-second observation with
  zero applied frames. Actor playback and replay each completed all 24 frames;
  Details reported source frame 24 and `24/24`. Rendered completion took
  approximately 0.808 and 0.810 seconds.
- A third play stopped after applying source frame 1. UE reported Stopped,
  `1/24`, and retained the cache. Recapture on that same session then completed
  without a clear-ack timeout.
- Custom range on: `[-1, -1]` produced one frame at 30 fps. Ready again held
  without autoplay. Both play and replay completed `1/1`, with a valid applied
  source frame of -1. The actual Details formatter returned
  `源帧 -1–-1 · 30 fps · 当前已应用帧 -1 · 1/1 帧`.
- Returning to real-time changed UE to Idle. A subsequent Maya pose edit was
  evaluated by the displayed character. The rendered check compared all 872
  required bones; the returned-live maximum displayed position error was
  approximately 0.000036 cm, below the 0.1 cm acceptance tolerance.
- Base Color viewport screenshots were captured and inspected for the real
  character, including default completion/replay and negative-frame completion/
  replay. Body, clothing, head and hair remained assembled without visible
  gross deformation in these tested poses.
- The complementary synthetic reconnect run independently observed custom
  source frames -1, 0 and 1 as UE root X values -10, 0 and 10. The retained-cache
  replay preserved the original four poses after reconnect despite a Maya edit.

### Performance and limits

In the C01 NullRHI measurement, playback and replay each applied 24 frames at
approximately 29.84 and 29.86 fps for the captured 30 fps rate. The measured
Game Thread update/evaluation cost was mean 0.888 ms and p95 1.085 ms. This is
an isolated update/evaluation measurement, not rendered editor throughput.
The rendered 24-frame workflow also completed without a performance failure.
These results cover this character, outfit and short range; they are not a
benchmark guarantee for other scenes or long captures.

The Maya process used real scene evaluation, controller and socket transport;
its timer callbacks were driven on the mayapy main thread. The rendered UE
run exercised public Source/Actor operations and the same Details formatter
used by the UI, rather than manual button clicking. Optional fault paths,
clear-ID races, cancellation, resource limits and stale-session isolation are
covered by the passing unit/Automation regressions and prior code review.
UE asset warnings report existing assets saved without an engine version;
Maya host tests report the bundled dependency's `imp` deprecation warning.
Neither caused an acceptance failure in the final runs.

### Reproduction records

Local evidence is retained under the repository-adjacent
`../.tmp/mtou-issue49-acceptance/`: `suite-report`, `parts-final-report`,
`parts-final`, `render-report`, `render`, build logs and the C01 fixture.
`render/cache-*.json` records state, source range/rate, applied frame identity
and exact Details text; the matching PNG files are Base Color screenshots.
The scratch plugin differs from HEAD only in the rendered acceptance test,
which adds the cache sequence to the existing character harness. Product
sources remain unchanged. The temporary host and harness are retained with
the evidence for reproducibility and are not repository or release artifacts.
