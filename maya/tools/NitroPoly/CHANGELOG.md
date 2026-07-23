# Changelog

## Unreleased

- Flatten the Maya package into a root `scripts/` directory.
- Make the single runtime file launch when run directly.
- Document the optional shelf launcher without packaging a second runtime file.
- Encode non-ASCII UI text as Unicode escapes so Maya 2020's Script Editor can
  execute the single file without corrupting UTF-8 string literals.
- Remove the unnecessary Maya module file.

## 4.0.4

- Preserve the existing reconstructed NitroPoly implementation.
- Add a Maya module package and documented bootstrap entry point.
