# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [Unreleased]

- Fix unit-test linking when the OpenGL shader backends are disabled.
- Configure and game Properties: **Share settings** copies a tab or all settings as a short code and pastes one back, setting that tab to match after a preview; nothing is saved until OK or Apply, and GPU, audio devices, paths and accounts are never included. See `docs/user/Settings.md`.

## [0.1.0]

- Initial release candidate
