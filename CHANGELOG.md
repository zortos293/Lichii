# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [Unreleased]

- Fetch official Nintendo store artwork for games without a ROM icon, falling back to
  SteamGridDB when its API key is set, and cache the result locally.
- Use the configured system language for localized ROM icons and store covers, with a
  per-language artwork cache.
- Fix unit-test linking when the OpenGL shader backends are disabled.

## [0.1.0]

- Initial release candidate
