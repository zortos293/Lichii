# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [Unreleased]

- F7 (hotkey "Toggle NVIDIA DLSS") switches NVIDIA DLSS on and off while a game runs, to compare it
  with the game's own image
- Graphics: "NVIDIA DLSS Sharpness" (RCAS on DLSS's image, off by default, changes apply at once)
- Ocarina of Time: NVIDIA DLSS uses model M, and DLAA also runs with resolution scaling above 1x
  (at the scaled size, then box-filtered to the game's image)
- The FXAA or SMAA setting is skipped while NVIDIA DLSS draws the game's image (on top of DLSS
  they only blurred it); it applies again when DLSS is off

## [0.1.0]

- Initial release candidate
