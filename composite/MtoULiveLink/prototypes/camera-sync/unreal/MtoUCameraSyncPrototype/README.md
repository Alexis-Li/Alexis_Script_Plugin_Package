# MtoUCameraSyncPrototype (Unreal side)

Editor-only prototype module used by the Issue 52 verification. It evaluates the
current Unreal camera and publishes it; it is not part of the MtoULiveLink
product and ships with nothing.

## What it does

- Owns one `ULevelSequencePlayer` for the sequence under test and drives its
  time: `Play`, `Pause`, `Seek <display frame>`, play rate, looping. Unreal is
  the only writer of that time.
- Reads the evaluated camera from the engine
  (`ULevelSequencePlayer::GetActiveCameraComponent`), so camera cuts and
  subsequence shots are resolved by the engine, not by the prototype.
- Reads the camera exactly as it renders: `UCameraComponent::GetCameraView`
  plus the view-projection matrix at the configured output resolution, the
  aperture rectangle inside that resolution, and the projected positions of
  every actor tagged `MtoUCameraSyncMarker`.
- Serves one Maya client on `127.0.0.1:54330` with the schema in
  `../protocol.md`, refuses any message that tries to control time or camera,
  and keeps every `applied` report for the verification evidence.

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
MtoUCameraSyncPrototype.Start /Game/Cinematics/LS_Shot 54330 1920 1080
MtoUCameraSyncPrototype.Status
MtoUCameraSyncPrototype.Seek 1030
MtoUCameraSyncPrototype.Play 1.0
MtoUCameraSyncPrototype.Pause
MtoUCameraSyncPrototype.Stop
```

## Automation tests

| Test | Covers |
| --- | --- |
| `MtoUCameraSyncPrototype.CameraPayload` | Evaluated camera fields, film aperture and sensor offsets, evaluated depth of field values, the aperture rectangle inside the output resolution, marker projection and pixel mapping |
| `MtoUCameraSyncPrototype.CameraCutsAndTime` | Two camera cuts, non-zero playback start, reported seconds, a rejected client time request, playback and looping inside the range |
| `MtoUCameraSyncPrototype.SubsequenceTime` | A master sequence with a subsequence shot, evaluated through the engine while the master time stays authoritative |
| `MtoUCameraSyncPrototype.RealMayaPeer` | Opt-in: drives the Maya peer over a real socket. Requires `-MtoUCameraSyncMayapy=`, `-MtoUCameraSyncPeer=` and `-MtoUEvidence=` |

Run them with:

```
<Engine>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe <repo>/unreal/ToolsLab.uproject -unattended -nop4 -nosplash -NullRHI -DDC-ForceMemoryCache -ExecCmds="Automation RunTests MtoUCameraSyncPrototype" -TestExit="Automation Test Queue Empty"
```

## Limits

- The module supports perspective cameras only; orthographic cameras are
  refused with a message.
- The output resolution is a prototype input; wiring it to Movie Render
  Pipeline settings is not implemented.
- The prototype never touches the level or the sequence asset: it spawns a
  transient sequence player, and stopping the session destroys it and restores
  any pre-animated state.
- Nothing here is packaged, versioned or documented as a product feature.
