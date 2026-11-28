# CI and release guide

## Lichii release builds

Run `release-binaries` on the desired branch. Set `publish_release`, `include_whats_new`, and `nsight_aftermath` to false for an unpublished test of the full release matrix. The PGO mismatch summary runs only for the Linux x86_64 and Windows x86_64 PGO jobs. It counts compiler warnings, including how many report discarding nonzero counts and, for Linux, how many warn that a function has no profile data.

Linux ARM64 PGO installations with the fallback updater (starting with build 116) select the standard ARM64 package on their next update to a release without PGO: AppImage for AppImage installations, `.tar.gz` for unpacked installations. A writable AppImage can be replaced in place; a tarball must be unpacked manually. Builds 75 and 83 match only PGO assets and can first update through an existing PGO release such as [build 386](https://github.com/zortos293/Lichii/releases/tag/build-386) while the updater still lists it. If no update appears, or if you use build 70, which shipped without an updater, download the standard ARM64 package from [GitHub Releases](https://github.com/zortos293/Lichii/releases).

## Eglantine pipeline design (not Lichii's active workflow)

This document describes the split GitHub Actions pipeline added with this change. It is not yet a live workflow until the files have been merged to the required branches and the repository settings below have been applied.

## What runs when

| Event                                                  | What it builds                                                        | Publishes?                                                                 |
| ------------------------------------------------------ | --------------------------------------------------------------------- | -------------------------------------------------------------------------- |
| Push to `nx2` that changes `VERSION` or `CHANGELOG.md` | Stable release or release candidate, based on the suffix in `VERSION` | Yes, unless the exact tag already exists or the changelog section is empty |
| Scheduled run at 03:00 UTC daily                       | Latest `nx2` commit                                                   | Yes; skipped when the latest daily already points at that commit           |
| Actions → `ci` → Run workflow, `kind: branch`          | The selected branch                                                   | Yes by default; can be disabled                                            |
| Actions → `ci` → Run workflow, `kind: daily`           | `nx2` only, even if another branch is selected the run is rejected    | Yes by default; can be disabled                                            |
| Same-repository PR with label `ci:build`               | PR head commit; rebuilt on synchronize/reopen                         | Yes, as a rolling branch pre-release                                       |
| Fork PR                                                | No build from the base repository                                     | No; see “Forks” below                                                      |

**Important behavior change:** the previous `release-binaries.yml` built on selected source/build-file pushes to `nx2` and `nx2-experimental`. This design instead runs the release pipeline on `VERSION` or `CHANGELOG.md` changes, scheduled daily, manual requests, or explicitly labelled same-repository PRs. Ordinary source pushes do not trigger this release pipeline. A changelog-only push retries a previously blocked version release; once its tag exists, the duplicate check skips it. The `nx2-experimental` branch is not included in the new release triggers.

The schedule is defined in UTC, and GitHub schedules are not guaranteed to start at the exact minute. On public repositories, GitHub can disable scheduled workflows after 60 days without repository activity; high Actions load can also delay runs. The scheduled workflow runs from the repository's default branch, then checks out `nx2` for the actual build. See the one-time setup section: the reusable workflow files must also exist on the default branch.

**First-merge warning:** adding `ci.yml` and `VERSION` together to `nx2` triggers the initial version workflow. The supplied `CHANGELOG.md` intentionally leaves `[0.1.0]` empty rather than inventing release notes, so the first run fails safely at preflight until maintainers add real notes. Add the notes before the cutover merge to publish immediately, or add them in a follow-up changelog-only commit; `CHANGELOG.md` is also a push trigger for this recovery case. Confirm `VERSION`, permissions, and whether the first run should publish before merging the cutover.

## Version and changelog rules

`VERSION` is the single source of truth. It must contain exactly one version on one line:

```text
X.Y.Z
X.Y.Z-rcN
```

Examples: `0.1.0-rc1`, `0.1.0`, `0.1.1-rc1`. Numeric components cannot have leading zeroes. The bundle starts with `0.1.0-rc1` as a **suggested starting point**, not as an existing Eglantine version convention. The current public releases use `build-<run number>`; confirm the first semantic version with maintainers before merging this value. The included `[0.1.0]` changelog section is intentionally empty and must receive the real initial release notes before the first versioned release can publish.

Add changes to the top `## [Unreleased]` section in `CHANGELOG.md`. When a new candidate is prepared, the bump script moves those entries beneath `## [X.Y.Z]` and leaves an empty `## [Unreleased]` section ready for the next cycle. The RC and stable release notes come from that version section; an absent or empty section fails before the build is published.

### Creating a release

1. For the initial `0.1.0-rc1`, add the real candidate notes under `## [0.1.0]`; after the first stable release, add new notes under `## [Unreleased]` and commit them to `nx2`.
2. Open Actions → `bump-version` → Run workflow. Choose one of:
   - `patch`: `0.1.0` → `0.1.1-rc1`.
   - `minor`: `0.1.0` → `0.2.0-rc1`.
   - `major`: `0.1.0` → `1.0.0-rc1`.
   - `rc`: `0.2.0-rc1` → `0.2.0-rc2`.
   - `release`: `0.2.0-rcN` → `0.2.0`.
3. The workflow commits the changed `VERSION` and `CHANGELOG.md` to `release/vX.Y.Z[-rcN]` and opens (or updates) a PR into `nx2`.
4. Review and merge the PR. The merge push changes `VERSION`, which starts `ci`.
5. Test the RC binaries before selecting `release` to promote that candidate to stable.

The bump workflow does not merge the PR for you. The merge is the approval gate. Merge or close the current release PR before starting another bump; otherwise the next run starts again from the still-unmerged `nx2` version. The helper script refuses to start a new major/minor/patch candidate while the current `VERSION` is still an RC; finish or promote that RC first. If branch protection requires CI on release PRs, add `ci:build` after reviewing the generated PR so it gets a CI run.

The CI duplicate check refuses to publish a tag that already exists. It does **not** compare the new semantic version numerically with the latest release, so maintainers must keep version numbers moving forward. If a failed run did not create the release/tag, re-run the workflow after fixing the failure.

## Running a build manually

Open Actions → `ci` → Run workflow. Select the branch to build and choose:

- **kind**: `branch` for a test build, or `daily` for a forced daily build of `nx2` only.
- **platforms**: `all`, `linux`, `windows`, or `macos`. A single-platform run produces a partial release with only that platform's assets.
- **pgo**: `none` or `both`. For manual runs, `none` is the default. `both` adds the Linux x86_64 and Windows x86_64 PGO variants. Linux ARM64 and macOS have no PGO variant.
- **publish**: enabled by default. Disable it to keep the build as workflow artifacts without creating/updating a GitHub release.

For automatic `nx2` version releases and scheduled daily builds, the workflow requests both PGO variants by default. Label-triggered PR builds default to non-PGO to keep test builds quicker.

A manual `daily` run is restricted to the `nx2` ref. It forces a fresh daily build for that date; with `publish: true`, it replaces the existing `daily-YYYYMMDD` release for the same day.

## Pull request and branch builds

For a same-repository PR, a maintainer should add the `ci:build` label only after reviewing the source as trusted. A build runs when the label is added, when the PR is synchronized with new commits, and when it is reopened (as long as the label remains). It checks out the PR head commit rather than GitHub's synthetic merge commit. The release job needs `contents: write` to create the rolling branch release, so the label is an explicit trust/approval boundary; do not use it as an automatic label for untrusted contribution code. The optional Discord webhook is not passed to PR-triggered release calls. The branch release is named `branch-<slug>-<short-hash>` and is deleted when the PR closes or its branch is deleted. A weekly sweep removes branch releases whose source branches no longer exist.

The hash suffix makes the generated branch tag collision-resistant even when two branch names normalize to the same text. Repeated builds of the same branch update the same rolling pre-release; older branch assets are replaced rather than accumulated.

### Forks

The base repository does not build or publish fork PR code. This is intentional: the workflow would otherwise execute untrusted code in a repository context with access to repository-level publishing permissions. A contributor who needs a build can enable Actions in their fork and run `ci` manually there; the release, if published, belongs to that fork.

Both the scheme-preparation job and the publishing job check out CI helper scripts from the repository's default branch, not from the branch/PR commit being packaged. The preparation job fetches the target commit as Git data and reads `VERSION` from it only for a version-triggered push. The publishing job fetches the target commit for Git history and, for stable/RC notes, reads that commit's `CHANGELOG.md`. Do not change this separation: the release job has `contents: write` and must not execute scripts supplied by PR/branch build commits.

## Build outputs

| Platform            | Default output               | Optional PGO output    |
| ------------------- | ---------------------------- | ---------------------- |
| Linux x86_64        | AppImage and `.tar.gz`       | AppImage and `.tar.gz` |
| Linux aarch64       | AppImage and `.tar.gz`       | Not built              |
| Windows x86_64      | clang-cl + ThinLTO `.zip`    | clang-cl + ThinLTO + PGO `.zip` |
| macOS Apple Silicon | App bundle `.zip` and `.dmg` | Not built              |

The existing build steps use the repository's current CMake targets (`lichii` and `lichii-cmd`) and package the produced `eglantine` and `eden-cli` binaries. Linux AppImages require glibc 2.39 or newer. Linux tarballs hold the same files as the AppImage, unpacked, with `lichii` and `eden-cli` links at the top. The standard Windows build uses clang-cl from a pinned LLVM release (`tools/windows/install-llvm.ps1`, shared with the Windows build-check job) in Release mode with ThinLTO, against the MSVC headers, runtime and SDK. MSVC itself still builds the project for development but is not used for releases. Windows archives contain the deployed runtime dependencies. macOS artifacts are ad-hoc signed, not notarized; Gatekeeper may require the user to approve the app on first launch.

Build artifacts are retained for seven days. Published GitHub Release assets remain available until a release is deleted. Daily releases are pruned to approximately the seven most recent; branch releases are rolling and are cleaned up when no longer needed.

## Build caches

`build-check.yml` and `release-binaries.yml` cache compiler objects (ccache on every platform, plus lld's ThinLTO cache for the Windows release builds), CPM downloads, the x86-64 release jobs' apt packages and fsr4-vk. A run can restore caches saved on its own ref and on `nx2`, the default branch, and the repository holds 10 GB of caches before GitHub evicts the least recently used.

So only `nx2` saves compiler, CPM and apt caches. Pull requests and branch release builds restore `nx2`'s entries and save none: their own entries helped only that ref, and with one per pull request push they filled the 10 GB and evicted the release caches, whose builds then compiled from scratch. After each save, `tools/ci/prune-caches.sh` deletes the older `nx2` entries of the same family, and each build evicts the ccache entries it didn't use, so a family's entry stays one build's size. A run on `nx2` doesn't save when it was cancelled.

The nightly `release-binaries` run (03:17 UTC) builds `nx2` without publishing to keep those caches close to `nx2`'s head. It skips a commit that already has a successful `nx2` build.

## Linux PGO profile

`linux-x86_64-pgo` is the `linux-x86_64` build compiled by clang (`PGO_LLVM_VERSION`) with `-fprofile-use=lichii-linux.profdata`. That profile is `eglantine.profdata`, weight 1, merged with an indexed profile of two sessions of about five minutes of Ocarina of Time gameplay on x86_64 Linux (a Steam Deck), weight 12, so the Switch 2 paths Ocarina reaches get most of the weight and Eglantine's records keep some where they still match. Eglantine's were trained against libstdc++ 14 or later (they include `std::vector::_M_realloc_append`, which GCC 13's headers lack), so with the job pinned to GCC 13 those whose inlined library code differs no longer match and add nothing. `eglantine.profdata` is no longer read by any build; it stays in the repository as the input for retraining.

A profile names a file-local function (static, or in an anonymous namespace) by the path of its source file. The job and the training build therefore both pass `-mllvm -static-func-strip-dirname-prefix=N`, with N computed in the source root as

```sh
strip_n=$(( $(printf '%s' "$PWD" | tr -cd / | wc -c) + 1 ))
```

so those names start at `src/` and a profile trained in any checkout matches the runner's. Without it about a fifth of the profile's functions (14,763 of the committed profile's 78,301 records) silently get no data. It is an internal LLVM option, verified with clang 23.1.6; if a later LLVM drops it, the build fails on the unknown option.

An older profile only produces warnings, and code it never reached is compiled as in the normal build. The job summary counts functions whose code changed since training (hash mismatches, template and inline functions included through `-mllvm -no-pgo-warn-mismatch-comdat-weak=false`, which changes only diagnostics) and missing-profile warnings (`-mllvm -pgo-warn-missing-function`, one per function and translation unit the profile has no entry for). It warns when more than `PGO_MISMATCH_MAX` (10) functions lost data that had counts, when the missing-profile count passes `PGO_MISSING_MAX` (3000), and when it is 0 (the flags that report them did not reach the build, so the summary would show nothing). The committed profile gives 1,282 missing-profile warnings and one function with counts lost (`lichii-cmd`'s `main`) on its own tree; `eglantine.profdata` gives 31,799. Training and the job must agree on three things, or functions lose their profile data as "hash mismatch" warnings:

- **The clang build.** The job installs `clang-$PGO_LLVM_VERSION` from apt.llvm.org, which keeps only the newest snapshot of each major, so the exact build cannot be pinned without the install failing once it moves. The job prints it and warns when it differs from `PGO_CLANG_BUILD`, the build that trained the profile; train with that same package (an Ubuntu 24.04 container with apt.llvm.org's `llvm-toolchain-noble-$PGO_LLVM_VERSION`), and set `PGO_CLANG_BUILD` to it when the profile is replaced.
- **The standard library headers.** A function's hash covers the std:: code inlined into it, and clang compiles against the newest GCC it finds. The runner image carries a newer GCC than Ubuntu 24.04's default, so the job passes `--gcc-install-dir` for GCC `PGO_GCC_MAJOR` (13) to compiling and linking, prints the GCC it selected, and fails early if that GCC's headers are missing. Train with the same: `--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13` in the training build's flags, in a container that has GCC 13. A release build of a fork of this job was compiled against libstdc++ 14 with a profile trained against 13 and lost the profile of 308 functions, among them the renderer's state updates and the macro engine.
- **The configuration.** The training build uses the job's CMake options and Release build type; only `LICHII_BUILD_TAG` and `LICHII_USE_PENDING_RELEASE_NOTES` may differ, as they change no code the profile covers.

**Measure the binary the job builds.** A measurement that backs a profile, or a change to how the job builds, is taken on a binary built with the job's own steps, on the job's runner image or in a container shown to hold the same toolchain: the same clang build, the same standard library headers, and the same source paths the profile keys use. The same flags on another machine are not enough. The fork's build above was measured on a build that used GCC 13's libstdc++, while the runner's clang used GCC 14's, and the measurement could not show the 308 functions the published package lost. Compare the job's PGO summary (hash mismatches and missing-profile warnings) with the measured build's: a difference means the measurement was not of the job's binary.

To retrain:

1. Configure a Release build as the `linux-x86_64-pgo` job does (build directory `build/release`), with clang `PGO_LLVM_VERSION` and `-fprofile-generate=<dir> -fprofile-update=atomic -mllvm -static-func-strip-dirname-prefix=$strip_n --gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13` in `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` in place of `-fprofile-use`, `strip_n` computed as above in the checkout's root, and build `lichii`.
2. Run it, play the scenes the profile should cover, and exit normally (counters are written at exit).
3. Merge with the same LLVM. `eglantine.profdata` names its local functions with its author's checkout path, so strip that first: `llvm-profdata merge -text -o eglantine.txt eglantine.profdata`, `sed -E 's#^/home/[^/]+/Eglantine/##' eglantine.txt > eglantine-rel.txt`, `llvm-profdata merge -o eglantine-rel.profdata eglantine-rel.txt`. Then `llvm-profdata merge -o ocarina.profdata <dir>/*.profraw` and `llvm-profdata merge -o lichii-linux.profdata -weighted-input=1,eglantine-rel.profdata -weighted-input=12,ocarina.profdata`, and replace the file in the repository root. `llvm-profdata show --all-functions lichii-linux.profdata | grep -c '^  /'` should print 0.

A profile ages with the tree: applied to a tree 20 upstream commits newer than the one it was trained on, the previous profile lost its data for 12 functions training had reached, among them the shader compiler's SSA rewrite pass and the Vulkan fence manager's release thread. Retrain when the summary's count of functions that lost data with counts passes `PGO_MISMATCH_MAX`, or names code that matters; that count, not the number of commits, is the signal.

## Windows PGO profile

`windows-x86_64-pgo` is the `windows-x86_64` build plus `-fprofile-use=lichii-windows.profdata`. The profile is an LLVM indexed IR profile from clang-cl, so its function names use the MSVC ABI. Linux's `eglantine.profdata` uses Itanium names, and clang-cl cannot use it. The committed profile comes from about six minutes of Ocarina of Time gameplay. Code that run never reached gets no profile data, so other games gain less.

A profile older than the source only produces warnings. Functions whose code changed since training lose their profile data and are compiled as in the ThinLTO build. Retrain when those warnings cover code that matters, or when the LLVM version changes. clang-cl reads only the indexed format its own LLVM writes, so use the version that `tools/windows/install-llvm.ps1` installs.

To retrain:

1. Configure a Release build as the `windows-x86_64` job does, adding `-fprofile-generate=<dir> -fprofile-update=atomic` to `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS`, and build `lichii`.
2. Run `lichii.exe` with `LLVM_PROFILE_FILE=<dir>/lichii-%m-%p.profraw`, play the scenes the profile should cover, and exit normally. Counters are written only at exit.
3. Merge with that LLVM: `llvm-profdata merge -o lichii-windows.profdata <dir>/*.profraw`.
4. Check that `llvm-profdata show --all-functions lichii-windows.profdata` lists names starting with `?`, then replace the file in the repository root.

## Release naming

| Kind              | Tag                    | Release title                            | GitHub Release flag                |
| ----------------- | ---------------------- | ---------------------------------------- | ---------------------------------- |
| Stable            | `vX.Y.Z`               | `Eglantine vX.Y.Z`                       | Not a pre-release; marked latest   |
| Release candidate | `vX.Y.Z-rcN`           | `Eglantine vX.Y.Z-rcN`                   | Pre-release                        |
| Daily             | `daily-YYYYMMDD`       | `Eglantine Build DD/MM/YYYY`             | Pre-release; replaces same-day tag |
| Branch / PR       | `branch-<slug>-<hash>` | `Eglantine Branch: <PR title or branch>` | Pre-release; rolling               |

Only stable and RC publications send the optional Discord notification. Daily and branch releases do not.

## How SCM version metadata reaches CMake

The local `setup-build` action writes the four root-level files consumed by the repository's `GetSCMRev` CMake module: `GIT-COMMIT`, `GIT-REFSPEC`, `GIT-TAG`, and (for stable/RC only) `GIT-RELEASE`. This avoids depending on tags being present in shallow checkouts or on a named branch being available in detached PR checkouts.

Stable and RC builds get `GIT-RELEASE`, so CMake marks them as release builds. Daily and branch builds omit that file and are configured with `NIGHTLY_BUILD=ON`; stable and RC builds are configured with it off. The project currently disables the update checker in the CI build flags. The SCM/update-server template still contains upstream Eden URLs; do not enable the updater until those URLs are intentionally migrated to Eglantine's own service.

## Troubleshooting

- **`ci` or `bump-version` is missing from Actions:** confirm its workflow file is on the repository's default branch.
- **Scheduled builds never appear:** confirm `ci.yml` is on the default branch, scheduled Actions are enabled, and the run has not been disabled after 60 days of public-repository inactivity. Schedule times can also be delayed by Actions load.
- **The version push is skipped:** check the `VERSION` syntax and whether `vX.Y.Z[-rcN]` already exists as a tag/release. A duplicate is skipped, not rebuilt.
- **Release notes step fails:** ensure the matching `## [X.Y.Z]` section in `CHANGELOG.md` contains at least one non-blank entry.
- **Bump workflow cannot create a PR:** enable “Allow GitHub Actions to create and approve pull requests”, or create the release PR manually.
- **Mac app fails on an older OS:** check the effective minimum OS of Homebrew dylibs. The project's CMake deployment target is 15.0, but dependencies built on a newer runner can raise the effective runtime requirement.
- **A branch pre-release remains after cleanup:** run Actions → `cleanup-branch` → Run workflow to sweep orphaned branch releases.
