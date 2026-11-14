#!/usr/bin/env bash
# Eglantine build script for Linux (Fedora, Debian/Ubuntu/Mint, Arch/CachyOS)
#
# Detects the distro and the host CPU's x86-64 level (v1/v2/v3/v4), resolves
# every dependency against the distro's own repositories (nothing is guessed:
# a package that does not exist is reported, not skipped), then builds with
# flags matched to that CPU.
#
# Usage:
#   ./build-linux.sh [options]
#
# Options:
#   --src DIR          Source directory (default: current directory)
#   --build-dir DIR    Build directory (default: <src>/build-<branch>-<level>)
#   --type TYPE        Release | RelWithDebInfo | Debug (default: Release)
#   --level N|auto     x86-64 level 1-4 (default: auto = highest the host runs)
#   --max-level N      Cap auto-detection (e.g. 3 on Skylake-X, where AVX-512
#                      can downclock the core)
#   --native           Use -march=native instead of an x86-64-vN level
#                      (fastest on this machine, binary is NOT portable)
#   --no-cpu-check     Skip host detection (cross-building); needs --level
#   --gcc              Use GCC instead of clang
#   --no-lto           Disable LTO
#   --clean            Wipe the build directory first
#   --install-deps     Install build dependencies (dnf / apt / pacman; on Arch
#                      this runs pacman -Syu, i.e. a full system upgrade)
#   --check-deps       Resolve and verify dependencies, then exit
#   --skip-dep-check   Do not abort if a dependency is not installed
#   --no-qt            Build without the Qt frontend (CLI only)
#   --werror           Keep -Werror on (off by default; new GCC/Clang trip it)
#   --system-vulkan    Use system Vulkan/SPIR-V packages
#   --bundled-vulkan   Force bundled Vulkan/SPIR-V (default on Fedora)
#   --prefix DIR       Install prefix; runs `cmake --install` after the build
#   -j N               Parallel jobs (default: min(nproc, RAM / per-job need))
#   -h, --help         Show this help
#
# Extra CMake arguments can be passed via EXTRA_CMAKE_ARGS, e.g.
#   EXTRA_CMAKE_ARGS="-DYUZU_TESTS=OFF" ./build-linux.sh
#
# Dependency lists come from the project's docs/Deps.md (nx2 branch). Where
# this script adds a package that Deps.md does not list, it is marked
# "extra" in the package tables below.

set -Eeuo pipefail

SRC_DIR="$PWD"
BUILD_DIR=""
BUILD_TYPE="Release"
LEVEL_REQ="auto"
MAX_LEVEL=4
USE_NATIVE=0
USE_CLANG=1
USE_LTO=1
CLEAN=0
INSTALL_DEPS=0
CHECK_DEPS_ONLY=0
SKIP_DEP_CHECK=0
CPU_CHECK=1
ENABLE_QT=ON
WERROR=OFF
BUNDLE_VULKAN=""        # empty = decide per distro
PREFIX=""
JOBS=""

# log MSG... - print a bold progress message to stdout.
log()  { printf '\033[1m==> %s\033[0m\n' "$*"; }
# warn MSG... - print a yellow warning to stderr.
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
# die MSG... - print a red error to stderr and exit 1.
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
# usage - print the header comment block of this script as help text and exit 0.
usage() { awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0; }

# need_value OPTION [VALUE] - die unless OPTION was given a non-empty value.
need_value() { [[ $# -ge 2 && -n "$2" ]] || die "option $1 requires a value"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --src|--build-dir|--type|--level|--max-level|--prefix|-j)
            need_value "$1" "${2-}" ;;
    esac
    case "$1" in
        --src)            SRC_DIR="$(realpath -m "$2")"; shift 2 ;;
        --build-dir)      BUILD_DIR="$(realpath -m "$2")"; shift 2 ;;
        --type)           BUILD_TYPE="$2"; shift 2 ;;
        --level)          LEVEL_REQ="${2#v}"; shift 2 ;;
        --max-level)      MAX_LEVEL="${2#v}"; shift 2 ;;
        --native)         USE_NATIVE=1; shift ;;
        --no-cpu-check)   CPU_CHECK=0; shift ;;
        --gcc)            USE_CLANG=0; shift ;;
        --clang)          USE_CLANG=1; shift ;;
        --no-lto)         USE_LTO=0; shift ;;
        --lto)            USE_LTO=1; shift ;;
        --clean)          CLEAN=1; shift ;;
        --install-deps)   INSTALL_DEPS=1; shift ;;
        --check-deps)     CHECK_DEPS_ONLY=1; shift ;;
        --skip-dep-check) SKIP_DEP_CHECK=1; shift ;;
        --no-qt)          ENABLE_QT=OFF; shift ;;
        --werror)         WERROR=ON; shift ;;
        --system-vulkan)  BUNDLE_VULKAN=0; shift ;;
        --bundled-vulkan|--bundle-vulkan) BUNDLE_VULKAN=1; shift ;;
        --prefix)         PREFIX="$(realpath -m "$2")"; shift 2 ;;
        -j)               JOBS="$2"; shift 2 ;;
        -h|--help)        usage ;;
        *)                die "unknown option: $1" ;;
    esac
done

[[ "$LEVEL_REQ" == auto || "$LEVEL_REQ" =~ ^[1-4]$ ]] || die "--level must be auto or 1-4"
[[ "$MAX_LEVEL" =~ ^[1-4]$ ]] || die "--max-level must be 1-4"
case "$BUILD_TYPE" in
    Release|RelWithDebInfo|Debug|MinSizeRel) ;;
    *) die "--type must be Release, RelWithDebInfo, Debug or MinSizeRel" ;;
esac
[[ -z "$JOBS" || "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "-j needs a positive integer"
[[ "$(uname -m)" == x86_64 ]] || die "this script targets x86_64 only (found $(uname -m))"

SUDO=""
((EUID == 0)) || SUDO="sudo"

# ------------------------------------------------------------ distro detect
FAMILY=""; DISTRO_ID=""; DISTRO_VER=""; DISTRO_NAME=""

# detect_distro - read /etc/os-release and set FAMILY, DISTRO_ID, DISTRO_VER, DISTRO_NAME.
detect_distro() {
    [[ -r /etc/os-release ]] || die "cannot read /etc/os-release"
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO_ID="${ID:-unknown}"
    DISTRO_VER="${VERSION_ID:-0}"
    DISTRO_NAME="${PRETTY_NAME:-$DISTRO_ID}"
    local like=" ${ID_LIKE:-} ${DISTRO_ID} "

    case "$DISTRO_ID" in
        rhel|centos|rocky|almalinux|ol)
            die "$DISTRO_NAME needs EPEL + CRB (see docs/Deps.md, RHEL section); not automated here" ;;
    esac

    case "$like" in
        *" fedora "*)           FAMILY=fedora ;;
        *" debian "*|*" ubuntu "*) FAMILY=debian ;;
        *" arch "*)             FAMILY=arch ;;
        *) die "unsupported distro: $DISTRO_NAME (supported: Fedora, Debian/Ubuntu/Mint, Arch/CachyOS)" ;;
    esac

    # Deps.md minimums for the Debian family. Package resolution below is the
    # real gate, so these are warnings, not hard stops.
    if [[ "$FAMILY" == debian ]]; then
        case "$DISTRO_ID" in
            debian) (( ${DISTRO_VER%%.*} >= 13 )) \
                      || warn "Deps.md requires Debian 13 or later (found $DISTRO_VER)" ;;
            ubuntu) [[ "$(printf '%s\n26.04\n' "$DISTRO_VER" | sort -V | head -n1)" == 26.04 ]] \
                      || warn "Deps.md requires Ubuntu 26.04 or later (found $DISTRO_VER)" ;;
            linuxmint) warn "Deps.md requires Linux Mint 22.3 or later (found $DISTRO_VER)" ;;
        esac
    fi
    log "Distro: $DISTRO_NAME (family: $FAMILY)"
}

# ------------------------------------------------------------------ packages
# Entries are package names; "a|b" means "first one the repos actually have".
# Tiers:
#   REQ  - from docs/Deps.md (or the build toolchain); build aborts if one is
#          not available in the enabled repositories
#   REC  - not in Deps.md for this distro, but present in another distro's
#          list or needed for a working Wayland/translation build; installed
#          when available, a warning if not
declare -a REQ=() REC=() QT_REQ=() QT_REC=()

# The build below passes YUZU_TESTS=OFF, so Catch2 is only needed when
# EXTRA_CMAKE_ARGS turns a test suite back on (EXTRA_CMAKE_ARGS is appended
# last, so it overrides the OFF). Both -DX=ON and -DX:BOOL=ON are recognised.
tests_enabled() {
    [[ "${EXTRA_CMAKE_ARGS:-}" =~ -D(YUZU_TESTS|DYNARMIC_TESTS)(:[A-Za-z]+)?=(ON|on|TRUE|true|YES|yes|Y|y|1)([[:space:]]|$) ]]
}

# define_packages - fill REQ/REC/QT_REQ/QT_REC for the detected FAMILY (and Qt/test options).
define_packages() {
    local catch2_pkg=catch2   # Fedora ships Catch2 as catch-devel
    case "$FAMILY" in
    debian)
        REQ=(
            # Deps.md (Ubuntu, Debian, Mint)
            autoconf cmake g++ gcc git glslang-tools libglu1-mesa-dev libhidapi-dev
            libpulse-dev libtool libudev-dev libxcb-icccm4 libxcb-image0
            libxcb-keysyms1 libxcb-render-util0 libxcb-xinerama0 libxcb-xkb1
            libxext-dev libxkbcommon-x11-0 mesa-common-dev nasm ninja-build
            libfmt-dev liblz4-dev nlohmann-json3-dev libzstd-dev libssl-dev
            libavfilter-dev libavcodec-dev libswscale-dev pkg-config zlib1g-dev
            libva-dev libvdpau-dev libvulkan-dev spirv-tools spirv-headers
            libusb-1.0-0-dev libxbyak-dev libboost-dev libboost-fiber-dev
            libboost-context-dev libsdl3-dev libopus-dev
            "libasound2t64|libasound2" vulkan-utility-libraries-dev
            # extra: toolchain used by this script
            clang lld ccache python3 patch jq
        )
        QT_REQ=( qt6-base-private-dev qt6-tools-dev qt6-charts-dev )   # Deps.md
        QT_REC=( qt6-multimedia-dev qt6-wayland qt6-l10n-tools )       # extra
        ;;
    fedora)
        REQ=(
            # Deps.md (Fedora)
            autoconf cmake fmt-devel gcc gcc-c++ glslang hidapi-devel json-devel
            libtool libusb1-devel libzstd-devel lz4-devel nasm ninja-build
            openssl-devel pulseaudio-libs-devel speexdsp-devel wayland-devel
            zlib-devel libXext-devel boost jq
            # ffmpeg-devel is RPM Fusion; ffmpeg-free-devel is Fedora's own
            "ffmpeg-devel|ffmpeg-free-devel"
            # extra: headers for boost, toolchain used by this script
            boost-devel git clang lld ccache python3 patch
        )
        # Deps.md writes qt6-charts-devel; Fedora's package is normally
        # qt6-qtcharts-devel. Whichever the repos have is used.
        QT_REQ=(
            qt6-linguist qt6-qtbase-devel qt6-qtbase-private-devel
            qt6-qtwebengine-devel qt6-qtmultimedia-devel
            "qt6-qtcharts-devel|qt6-charts-devel"
        )
        QT_REC=( qt6-qtwayland )                                       # extra
        ;;
    arch)
        # Deps.md Arch list; Catch2 is added below only when tests are enabled.
        REQ=(
            base-devel boost cmake enet ffmpeg fmt git glslang libzip lz4
            ninja nlohmann-json openssl opus sdl3 zstd zip unzip
            vulkan-headers vulkan-utility-libraries libusb spirv-tools
            spirv-headers ccache clang lld python
        )
        QT_REQ=( qt6-base qt6-multimedia qt6-charts )
        QT_REC=( qt6-wayland )
        # CachyOS & co. ship zlib-ng-compat, which provides zlib but conflicts
        # with the stock package: only request zlib if nothing provides it.
        pacman -T zlib >/dev/null 2>&1 || REQ+=(zlib)
        ;;
    esac
    if tests_enabled; then   # only when a test suite is enabled
        [[ "$FAMILY" == fedora ]] && catch2_pkg=catch-devel
        REQ+=("$catch2_pkg")
    fi
    if [[ "$ENABLE_QT" == ON ]]; then
        REQ+=("${QT_REQ[@]}"); REC+=("${QT_REC[@]}")
    fi
}

# apt_avail PKG - succeed if apt has an installable candidate for PKG.
apt_avail() {
    local c
    c="$(apt-cache policy "$1" 2>/dev/null | awk '/Candidate:/ {print $2}')" || true
    [[ -n "$c" && "$c" != "(none)" ]]
}
# rpm_avail PKG - succeed if PKG (or a provider) is installed or available through dnf.
rpm_avail() {
    rpm -q --whatprovides "$1" >/dev/null 2>&1 && return 0
    [[ -n "$(dnf -q repoquery --whatprovides "$1" 2>/dev/null | head -n1)" ]]
}
# pkg_avail PKG - dispatch to the availability check for the current FAMILY.
pkg_avail() {
    case "$FAMILY" in debian) apt_avail "$1" ;; fedora) rpm_avail "$1" ;; esac
}
# pkg_installed PKG - succeed if PKG (or a provider of it) is already installed.
pkg_installed() {
    case "$FAMILY" in
        debian) [[ "$(dpkg-query -W -f='${db:Status-Status}' "$1" 2>/dev/null)" == installed ]] ;;
        fedora) rpm -q --whatprovides "$1" >/dev/null 2>&1 ;;
    esac
}

R_OK=(); R_MISSING=()
# resolve ENTRY... - pick the first available "a|b" alternative per entry; sets R_OK and R_MISSING.
resolve() {   # resolve entries -> R_OK (chosen names) / R_MISSING (entries)
    R_OK=(); R_MISSING=()
    local e alt hit
    local -a alts
    for e in "$@"; do
        hit=""
        IFS='|' read -r -a alts <<<"$e"
        for alt in "${alts[@]}"; do
            if pkg_avail "$alt"; then hit="$alt"; break; fi
        done
        if [[ -n "$hit" ]]; then R_OK+=("$hit"); else R_MISSING+=("$e"); fi
    done
}

# refresh_metadata - refresh apt/dnf package metadata for the current FAMILY.
refresh_metadata() {
    case "$FAMILY" in
        debian) log "Refreshing apt metadata"; $SUDO apt-get update ;;
        fedora) log "Refreshing dnf metadata"; $SUDO dnf -q makecache ;;
    esac
}

# missing_hint - print a distro-specific hint for why required packages may be missing.
missing_hint() {
    case "$FAMILY" in
        fedora) echo "  hint: ffmpeg-devel needs RPM Fusion; other misses usually mean the release is older than Deps.md assumes." ;;
        debian) echo "  hint: Deps.md needs Ubuntu 26.04 / Mint 22.3 / Debian 13 or newer (libsdl3-dev, libxbyak-dev, vulkan-utility-libraries-dev)." ;;
    esac
}

RESOLVED_REQ=(); RESOLVED_REC=()
# setup_deps - resolve, install (when requested) and verify required/optional packages.
setup_deps() {
    define_packages

    if [[ "$FAMILY" == arch ]]; then
        if ((INSTALL_DEPS)); then
            log "Installing build dependencies (pacman)"
            $SUDO pacman -Syu --needed --noconfirm "${REQ[@]}" "${REC[@]}"
        fi
        local miss
        miss="$(pacman -T "${REQ[@]}" 2>/dev/null || true)"
        if [[ -n "$miss" ]]; then
            ((SKIP_DEP_CHECK)) || die "missing packages (run with --install-deps): $(echo "$miss" | tr '\n' ' ')"
            warn "missing packages ignored: $(echo "$miss" | tr '\n' ' ')"
        fi
        return
    fi

    ((INSTALL_DEPS)) && refresh_metadata

    resolve "${REQ[@]}"
    RESOLVED_REQ=("${R_OK[@]+"${R_OK[@]}"}")
    if ((${#R_MISSING[@]})); then
        warn "required packages not available in the enabled repositories:"
        printf '    %s\n' "${R_MISSING[@]}" >&2
        missing_hint >&2
        die "fix the repositories (or the distro version); not continuing with guessed substitutes"
    fi

    resolve "${REC[@]+"${REC[@]}"}"
    RESOLVED_REC=("${R_OK[@]+"${R_OK[@]}"}")
    ((${#R_MISSING[@]})) && warn "optional packages not available, skipped: ${R_MISSING[*]}"

    if ((INSTALL_DEPS)); then
        log "Installing ${#RESOLVED_REQ[@]} required + ${#RESOLVED_REC[@]} optional packages"
        case "$FAMILY" in
            debian) $SUDO apt-get install -y "${RESOLVED_REQ[@]}" "${RESOLVED_REC[@]+"${RESOLVED_REC[@]}"}" ;;
            fedora) $SUDO dnf install -y "${RESOLVED_REQ[@]}" "${RESOLVED_REC[@]+"${RESOLVED_REC[@]}"}" ;;
        esac
    fi

    local p; local -a notinst=()
    for p in "${RESOLVED_REQ[@]}"; do pkg_installed "$p" || notinst+=("$p"); done
    if ((${#notinst[@]})); then
        ((SKIP_DEP_CHECK)) || die "not installed: ${notinst[*]} (run with --install-deps)"
        warn "not installed (ignored): ${notinst[*]}"
    else
        log "All ${#RESOLVED_REQ[@]} required packages are installed"
    fi
}

# ---------------------------------------------------------------- CPU level
# cpu_flags - print the CPU feature flags from /proc/cpuinfo.
cpu_flags() { grep -m1 '^flags' /proc/cpuinfo | cut -d: -f2; }

# has_all FLAGS NAME... - succeed if every NAME is in FLAGS (abm also matches lzcnt).
has_all() {   # has_all FLAGS name...   (lzcnt is reported as "abm" on many kernels)
    local flags=" $1 " f; shift
    for f in "$@"; do
        if [[ "$f" == abm ]]; then
            [[ "$flags" == *" abm "* || "$flags" == *" lzcnt "* ]] || return 1
        else
            [[ "$flags" == *" $f "* ]] || return 1
        fi
    done
}

# detect_host_level - print the host x86-64 microarchitecture level (1-4).
detect_host_level() {
    local ld lvl=0 out
    # glibc's own answer is authoritative (accounts for VMs masking features)
    for ld in /lib64/ld-linux-x86-64.so.2 /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2; do
        if [[ -x "$ld" ]]; then
            out="$("$ld" --help 2>/dev/null | grep -Eo 'x86-64-v[234] \(supported' || true)"
            if [[ -n "$out" ]]; then
                lvl="$(echo "$out" | grep -Eo 'v[234]' | tr -d v | sort -n | tail -n1)"
                echo "$lvl"; return
            fi
            [[ -n "$("$ld" --help 2>/dev/null | grep -F 'x86-64-v' || true)" ]] && { echo 1; return; }
        fi
    done
    # older glibc: derive from /proc/cpuinfo
    local f; f="$(cpu_flags)"
    lvl=1
    has_all "$f" cx16 lahf_lm popcnt sse4_1 sse4_2 ssse3 && lvl=2
    ((lvl == 2)) && has_all "$f" avx avx2 bmi1 bmi2 f16c fma abm movbe xsave && lvl=3
    ((lvl == 3)) && has_all "$f" avx512f avx512bw avx512cd avx512dq avx512vl && lvl=4
    echo "$lvl"
}

# cc_accepts COMPILER FLAG... - succeed if COMPILER compiles a trivial program with FLAG....
cc_accepts() {   # cc_accepts COMPILER flags...
    local cc="$1"; shift
    echo 'int main(void){return 0;}' | "$cc" "$@" -x c - -o /dev/null >/dev/null 2>&1
}

ARCH_FLAGS=""; LEVEL_LABEL=""
# select_arch_flags - set ARCH_FLAGS and LEVEL_LABEL from --level or the detected host level.
select_arch_flags() {
    local host want tune="generic" march

    if ((USE_NATIVE)); then
        ((CPU_CHECK)) || die "--native cannot be combined with --no-cpu-check"
        cc_accepts "$CC" -march=native || die "$CC rejects -march=native"
        ARCH_FLAGS="-march=native -mtune=native"; LEVEL_LABEL="native"
        return
    fi

    if ((CPU_CHECK)); then
        host="$(detect_host_level)"
        log "Host CPU level: x86-64-v$host ($(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs))"
    fi
    if [[ "$LEVEL_REQ" == auto ]]; then
        ((CPU_CHECK)) || die "--no-cpu-check needs an explicit --level"
        want="$host"
    else
        want="$LEVEL_REQ"
        if ((CPU_CHECK)) && ((want > host)); then
            die "host only runs x86-64-v$host; use --no-cpu-check to cross-build for v$want"
        fi
    fi
    if ((want > MAX_LEVEL)); then
        [[ "$LEVEL_REQ" == auto ]] || warn "--level $want capped to --max-level $MAX_LEVEL"
        want="$MAX_LEVEL"
    fi

    # Only ask the compiler for what it understands
    while :; do
        march="x86-64-v$want"; ((want == 1)) && march="x86-64"
        cc_accepts "$CC" "-march=$march" && break
        ((want > 1)) || die "$CC rejects -march=x86-64"
        warn "$CC does not support -march=$march; stepping down"
        want=$((want - 1))
    done

    # Tune for this exact microarchitecture, but only when it is the machine
    # that will run the binary
    if ((CPU_CHECK)) && cc_accepts "$CC" -mtune=native; then tune=native; fi

    ARCH_FLAGS="-march=$march -mtune=$tune"
    LEVEL_LABEL="v$want"
}

# ---------------------------------------------------------- toolchain / host
# check_toolchain - verify build tools exist and export CC/CXX for clang or gcc.
check_toolchain() {
    local tool
    for tool in cmake ninja python3; do
        command -v "$tool" >/dev/null || die "missing required tool: $tool (try --install-deps)"
    done
    local env_cc="${CC:-}" env_cxx="${CXX:-}"
    if ((USE_CLANG)); then
        # keep a user-supplied clang (e.g. CC=clang-19 CXX=clang++-19)
        if [[ -n "$env_cc" && -n "$env_cxx" ]] && command -v "$env_cc" >/dev/null \
            && "$env_cc" --version 2>/dev/null | grep -qi clang; then
            export CC="$env_cc" CXX="$env_cxx"
        else
            command -v clang >/dev/null || die "clang not found (try --install-deps, or use --gcc)"
            export CC=clang CXX=clang++
        fi
    else
        # an inherited CC that is really clang must not leak into a --gcc build
        if [[ -n "$env_cc" ]] && command -v "$env_cc" >/dev/null \
            && "$env_cc" --version 2>/dev/null | grep -qi clang; then
            env_cc=""; env_cxx=""
        fi
        command -v "${env_cc:-gcc}" >/dev/null || die "${env_cc:-gcc} not found (try --install-deps)"
        export CC="${env_cc:-gcc}" CXX="${env_cxx:-g++}"
    fi
}

LINK_ARGS=()
# pick_linker - choose lld (with clang) or mold when usable and fill LINK_ARGS.
pick_linker() {
    local ld="" is_clang=0
    # Ask the compiler itself; names lie (c++ is often GCC). lld cannot read
    # GCC's LTO objects, so it is only ever paired with a real clang.
    "$CXX" --version 2>/dev/null | grep -qi clang && is_clang=1
    if ((is_clang)) && command -v ld.lld >/dev/null && cc_accepts "$CC" -fuse-ld=lld; then
        ld=lld
    elif command -v mold >/dev/null && cc_accepts "$CC" -fuse-ld=mold; then
        ld=mold
    fi
    if [[ -n "$ld" ]]; then
        log "Linker: $ld"
        LINK_ARGS=(
            -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=$ld"
            -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=$ld"
            -DCMAKE_MODULE_LINKER_FLAGS="-fuse-ld=$ld"
        )
    fi
}

# pick_jobs - set JOBS from core count and RAM unless already given with -j.
pick_jobs() {
    [[ -n "$JOBS" ]] && return
    local cores mem_gb per_job=2 by_mem
    cores="$(nproc)"
    mem_gb=$(( $(awk '/MemTotal/ {print $2}' /proc/meminfo) / 1024 / 1024 ))
    ((USE_LTO)) && per_job=3
    by_mem=$(( mem_gb / per_job )); ((by_mem < 1)) && by_mem=1
    JOBS=$(( cores < by_mem ? cores : by_mem ))
    ((JOBS < cores)) && warn "limiting to $JOBS jobs (${mem_gb} GB RAM, ~${per_job} GB per job); override with -j"
    return 0
}

# ===================================================================== main
detect_distro
setup_deps
((CHECK_DEPS_ONLY)) && { log "Dependency check finished"; exit 0; }

check_toolchain
select_arch_flags
pick_linker
pick_jobs

[[ -f "$SRC_DIR/CMakeLists.txt" ]] || die "no CMakeLists.txt in $SRC_DIR (run from the repo root or use --src)"

# Build dir is named after the git branch so different branches never share a
# CMake cache: build-<branch>-<level>. Detached HEAD uses the short commit
# hash; a non-git source tree falls back to build-<level>.
git_branch_name() {
    local b
    command -v git >/dev/null || return 0
    b="$(git -C "$SRC_DIR" symbolic-ref --quiet --short HEAD 2>/dev/null)" \
        || b="$(git -C "$SRC_DIR" rev-parse --short HEAD 2>/dev/null)" || return 0
    # slashes and other odd characters become dashes (feature/x -> feature-x)
    printf '%s' "$b" | tr -c 'A-Za-z0-9._-' '-' | sed 's/^-*//; s/-*$//'
}
BRANCH="$(git_branch_name)"
if [[ -z "$BUILD_DIR" ]]; then
    if [[ -n "$BRANCH" ]]; then
        BUILD_DIR="$SRC_DIR/build-$BRANCH-$LEVEL_LABEL"
    else
        BUILD_DIR="$SRC_DIR/build-$LEVEL_LABEL"
    fi
fi
log "Building in $SRC_DIR${BRANCH:+ (branch: $BRANCH)}"

# --clean is destructive, so only ever remove a CMake build directory: no
# --build-dir pointing at the source tree (or a parent of it), and no existing
# directory that has never been configured by CMake.
cleanable() {
    local dir src
    dir="$(realpath -m "$1")"
    src="$(realpath -m "$2")"
    [[ -n "$dir" && "$dir" != "/" ]] || return 1
    [[ "$dir" == "$src" || "$src" == "$dir"/* ]] && return 1
    [[ -e "$dir" || -L "$dir" ]] || return 0   # does not exist yet
    [[ -d "$dir" && ! -L "$dir" ]] || return 1 # a file, a link, or something else
    [[ -f "$dir/CMakeCache.txt" || -f "$dir/build.ninja" ]] || return 1
}

if ((CLEAN)); then
    cleanable "$BUILD_DIR" "$SRC_DIR" \
        || die "refusing to delete $BUILD_DIR: not a CMake build directory"
    log "Cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# Fedora ships Vulkan-Headers without a matching Vulkan-Utility-Libraries,
# which CPM rejects as a partial install, so bundle the set there. The Debian
# list in Deps.md installs the matching system packages, so use those.
if [[ -z "$BUNDLE_VULKAN" ]]; then
    [[ "$FAMILY" == debian ]] && BUNDLE_VULKAN=0 || BUNDLE_VULKAN=1
fi

# YUZU_BUILD_PRESET only knows generic/v3/zen2/zen4/native, so keep it on
# "custom" (no -march injected) and pass the arch flags ourselves.
CMAKE_ARGS=(
    -S "$SRC_DIR"
    -B "$BUILD_DIR"
    -G Ninja
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_C_COMPILER="$(command -v "$CC")"
    -DCMAKE_CXX_COMPILER="$(command -v "$CXX")"
    -DYUZU_BUILD_PRESET=custom
    -DCMAKE_C_FLAGS="$ARCH_FLAGS"
    -DCMAKE_CXX_FLAGS="$ARCH_FLAGS"
    -DENABLE_QT="$ENABLE_QT"
    -DENABLE_WERROR="$WERROR"
    -DYUZU_TESTS=OFF
)
CMAKE_ARGS+=("${LINK_ARGS[@]+"${LINK_ARGS[@]}"}")

((BUNDLE_VULKAN)) && BUNDLE_ON=ON || BUNDLE_ON=OFF
CMAKE_ARGS+=(
    -DVulkanHeaders_FORCE_BUNDLED=$BUNDLE_ON
    -DVulkanUtilityLibraries_FORCE_BUNDLED=$BUNDLE_ON
    -DSPIRV-Headers_FORCE_BUNDLED=$BUNDLE_ON
    -DSPIRV-Tools_FORCE_BUNDLED=$BUNDLE_ON
)

# Also pass the "off" states: CMake keeps whatever a previous configure cached
# in a reused build directory, so --no-lto / --system-vulkan would otherwise
# stay on. EXTRA_CMAKE_ARGS is appended last, so it still wins.
if ((USE_LTO)); then CMAKE_ARGS+=(-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON)
else CMAKE_ARGS+=(-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DENABLE_LTO=OFF)
fi
if command -v ccache >/dev/null; then
    CMAKE_ARGS+=(
        -DCMAKE_C_COMPILER_LAUNCHER=ccache
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    )
else
    # Clear a launcher cached by an earlier run: the ccache binary may be gone
    CMAKE_ARGS+=(
        -DCMAKE_C_COMPILER_LAUNCHER=
        -DCMAKE_CXX_COMPILER_LAUNCHER=
    )
fi
[[ -n "$PREFIX" ]] && CMAKE_ARGS+=(-DCMAKE_INSTALL_PREFIX="$PREFIX")

# Split on whitespace only (no globbing); quoting inside the string is not parsed.
if [[ -n "${EXTRA_CMAKE_ARGS:-}" ]]; then
    read -r -a EXTRA_ARGV <<<"$EXTRA_CMAKE_ARGS"
    CMAKE_ARGS+=("${EXTRA_ARGV[@]}")
fi

# CMake caches the compiler on the first configure and ignores CC/CXX after
# that. If this build dir was configured with a different compiler (e.g. GCC
# earlier, clang now), drop the cache so the flags and linker below match.
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -n1)"
    if [[ -n "$cached_cxx" && "$(realpath -m "$cached_cxx")" != "$(realpath -m "$(command -v "$CXX")")" ]]; then
        warn "build dir was configured with $cached_cxx, now using $CXX; resetting CMake cache"
        rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"
    fi
fi

log "Configuring ($LEVEL_LABEL, $BUILD_TYPE, $CC, flags: $ARCH_FLAGS)"
cmake "${CMAKE_ARGS[@]}"

# Final guard: the compiler CMake actually used must be the one we planned for
used_cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -n1)"
[[ "$(realpath -m "$used_cxx")" == "$(realpath -m "$(command -v "$CXX")")" ]] \
    || die "CMake is using $used_cxx, expected $(command -v "$CXX"); rerun with --clean"

log "Building with $JOBS jobs"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

if [[ -n "$PREFIX" ]]; then
    log "Installing to $PREFIX"
    cmake --install "$BUILD_DIR"
fi

log "Build complete ($LEVEL_LABEL on $DISTRO_NAME)"
for bin in lichii eglantine eden eden-cli; do
    [[ -x "$BUILD_DIR/bin/$bin" ]] && echo "  $BUILD_DIR/bin/$bin"
done

# Optional binaries: listing them must not turn a good build into a failure
exit 0
