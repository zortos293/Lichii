# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [Unreleased]

- Fix unit-test linking when the OpenGL shader backends are disabled.
- Fire Emblem: Fortune's Weave (Vulkan): write back GPU-written images the texture cache drops,
  as for The Duskbloods, which fixes the striped sky and white bloom after leaving a menu (#1951).

## [0.1.0]

- Initial release candidate
