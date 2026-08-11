# Changelog

## Unreleased

- Consolidate the bilingual README, changelog, license, and project rules at
  the composite root while keeping host tests beside their component code.
- Keep packaged components limited to their host runtime files.

## 0.1.0 - 2026-08-11

- Provide the Maya 2022.4 sender and stock Unreal Editor 5.7.4 Live Link
  receiver as the first production-accepted MtoU_LiveLink release.
- Stream one evaluated deformation skeleton and matching BlendShapes from Maya.
- Merge same-named BlendShapes across skinned mesh parts when their evaluated
  values agree and report every conflicting plug when they differ.
- Receive native Live Link animation and curves while preserving placed actor
  transforms and without creating an Animation Sequence.
