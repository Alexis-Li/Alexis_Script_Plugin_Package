# Changelog

## Unreleased

- Define the protocol v2 wire fields, stable parser error categories, and
  connection-closing policy in one canonical cross-host conformance corpus.
- Exercise applicable canonical cases through both Maya and Unreal adapters,
  and fail repository validation when generated Unreal test data is stale.
- Keep the Maya sender window fitted to its controls so moving the window no
  longer alternates between a clipped and fully visible bottom section.
- Consolidate the bilingual README, changelog, license, and project rules at
  the composite root while keeping host tests beside their component code.
- Keep packaged components limited to their host runtime files.

## 0.2.0 - 2026-08-12

- Add a Chinese Maya UI with a red/green connection light, role setup,
  current-outfit, scene-frame-rate, streamed-bone, and BlendShape diagnostics.
- Detect the character Display controller and its Clothes enum, stream only
  effectively visible skinned meshes, and disconnect when the outfit changes.
- Sample at the Maya scene's exact 1-60 fps rate and rebuild the timer when the
  time unit changes.
- Upgrade the local protocol to version 2 with stable error codes, detailed
  skeleton diagnostics, bidirectional BlendShape differences, and an exactly
  one Binding Actor requirement.
- Add a Maya action that selects all joints involved in duplicate transmitted
  bone names by full DAG path for quick Outliner inspection.
- Allow duplicate Maya short bone names when Unreal can uniquely map its
  importer-added numeric suffixes within the corresponding parent branch.

## 0.1.0 - 2026-08-11

- Provide the Maya 2022.4 sender and stock Unreal Editor 5.7.4 Live Link
  receiver as the first production-accepted MtoU_LiveLink release.
- Stream one evaluated deformation skeleton and matching BlendShapes from Maya.
- Merge same-named BlendShapes across skinned mesh parts when their evaluated
  values agree and report every conflicting plug when they differ.
- Receive native Live Link animation and curves while preserving placed actor
  transforms and without creating an Animation Sequence.
