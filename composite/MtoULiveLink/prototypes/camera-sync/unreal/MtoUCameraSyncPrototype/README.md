# MtoUCameraSyncPrototype (Unreal side)

Editor-only prototype module used by the Issue 52 verification. It evaluates the
current Unreal camera and publishes it; it is not part of the MtoULiveLink
product and ships with nothing.

## What it does

- In editor mode, reads the open Sequencer's root time and last evaluated
  camera cut. Scrubbing, pausing, camera cuts and focused shots use the actual
  editor evaluation; the prototype never creates a second player in this mode.
- Keeps an isolated `ULevelSequencePlayer` mode for play, pause, seek, play rate
  and loop tests. Both modes use the engine's evaluated camera cut.
- Reads the camera exactly as it renders: `UCameraComponent::GetCameraView`
  plus the view-projection matrix at the configured output resolution, the
  aperture rectangle inside that resolution, and the projected positions of
  every actor tagged `MtoUCameraSyncMarker`.
- Serves one Maya client on `127.0.0.1:54330` with the schema in
  `../protocol.md`, refuses any message that tries to control time or camera,
  and keeps every `applied` report for the verification evidence. An opt-in
  keyed-joint witness from Maya is paired by session, serial, camera and time
  before moving a disposable test actor; stale reports are refused.

## Build and run

The prototype is loaded by the repository's Unreal test project
(`unreal/ToolsLab.uproject`), which lists
`composite/MtoULiveLink/prototypes/camera-sync/unreal` in
`AdditionalPluginDirectories` and enables the plugin:

```
<Engine>/Engine/Build/BatchFiles/Build.bat UnrealEditor Win64 Development <repo>/unreal/ToolsLab.uproject -WaitMutex -NoHotReloadFromIDE
```

In an interactive editor session:

```
MtoUCameraSyncPrototype.StartEditor 54330 1920 1080
MtoUCameraSyncPrototype.Status
MtoUCameraSyncPrototype.Stop
```

Open the Level Sequence editor first, then use its own playhead and transport.
`MtoUCameraSyncPrototype.Start /Game/Cinematics/LS_Shot 54330 1920 1080`
starts the isolated-player fixture instead; `Seek`, `Play` and `Pause` control
that fixture only.

## Automation tests

| Test | Covers |
| --- | --- |
| `MtoUCameraSyncPrototype.CameraPayload` | Evaluated camera fields, film aperture and sensor offsets, evaluated depth of field values, the aperture rectangle inside the output resolution, marker projection and pixel mapping |
| `MtoUCameraSyncPrototype.CameraCutsAndTime` | Two camera cuts, non-zero playback start, reported seconds, a rejected client time request, playback and looping inside the range |
| `MtoUCameraSyncPrototype.SubsequenceTime` | A master sequence with a subsequence shot, evaluated through the engine while the master time stays authoritative |
| `MtoUCameraSyncPrototype.EditorSequencer` | An open editor Sequence, two camera cuts, focused shot with root time, and no second time writer |
| `MtoUCameraSyncPrototype.RealMayaPeer` | Opt-in: follows the real editor with Maya over a socket, pairs one keyed-joint pose witness, rejects stale/reconnected reports and checks intact JSON lines. Requires `-MtoUCameraSyncMayapy=`, `-MtoUCameraSyncPeer=` and `-MtoUEvidence=` |
| `MtoUCameraSyncPrototype.TrailingByteCount` | A deliberately malformed UTF-8 line is counted in bytes, not characters |

Run them with:

```
<Engine>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe <repo>/unreal/ToolsLab.uproject -unattended -nop4 -nosplash -NullRHI -DDC-ForceMemoryCache -ExecCmds="Automation RunTests MtoUCameraSyncPrototype" -TestExit="Automation Test Queue Empty"
```

## Limits

- The module supports perspective cameras only; orthographic cameras are
  refused with a message.
- The output resolution is a prototype input; wiring it to Movie Render
  Pipeline settings is not implemented.
- The editor mode only observes the user's Sequencer, including its playhead;
  stopping follow leaves that editor time alone. The isolated-player mode
  destroys its transient player and restores pre-animated state.
- The joint witness does not use the MtoULiveLink product's pose channel, and
  continuous editor playback latency and a drop policy are not verified.
- Nothing here is packaged, versioned or documented as a product feature.
