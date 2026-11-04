#!/usr/bin/env bash
#
# Build and run the real server (Code/server) on Linux, in process, without a socket.
#
# This is the second half of the feedback loop. `run-core-tests.sh` covers protocol and
# coop-core rules; it never executes a line of `Code/server`, so server-side defects
# (ownership, damage, saves, parties - 14k lines) were only observable by playing the game
# after a 16-minute Windows build. This script compiles those sources with a plain compiler
# and drives them through the same dispatcher a network connection drives.
#
#   ./Tools/rfab/linux-server/run-server-tests.sh             # fetch (once), build, run
#   ./Tools/rfab/linux-server/run-server-tests.sh --objects   # compile the server only
#   ./Tools/rfab/linux-server/run-server-tests.sh --clean     # rebuild from scratch
#   JOBS=4 ./Tools/rfab/linux-server/run-server-tests.sh      # more parallel jobs
#
# Requirements: bash, curl, tar, make, g++ (C++20), gcc. Network only for the pinned
# dependency tarballs, and only on the first run.
#
# What is deliberately absent: GameNetworkingSockets (protobuf + abseil + OpenSSL) and the
# Skyrim client. Neither decides who owns an actor, so neither belongs in a test build.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DEPS_DIR="${REPO_ROOT}/build/deps"
JOBS="${JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 2 )}"

MODE="run"
for arg in "$@"; do
    case "${arg}" in
        --clean)   rm -rf "${REPO_ROOT}/build/linux-server" ;;
        --objects) MODE="objects" ;;
        *) echo "unknown argument: ${arg}" >&2; exit 2 ;;
    esac
done

fetch() {
    local name="$1" url="$2" expect="$3"
    if [[ -e "${DEPS_DIR}/${expect}" ]]; then
        return 0
    fi
    echo "==> fetching ${name}"
    mkdir -p "${DEPS_DIR}"
    curl -fsSL "${url}" -o "${DEPS_DIR}/${name}.tgz"
    mkdir -p "${DEPS_DIR}/${expect}"
    tar xzf "${DEPS_DIR}/${name}.tgz" -C "${DEPS_DIR}/${expect}" --strip-components=1
    rm -f "${DEPS_DIR}/${name}.tgz"
    [[ -e "${DEPS_DIR}/${expect}" ]] || { echo "!! ${expect} missing after ${name}" >&2; exit 1; }
}

# Versions are the ones pinned in xmake.lua. Picking "latest" is how a test build starts
# testing a different library than the shipping one: entt 3.13, for instance, no longer
# has `registry::ctx().at<T>()`, and half the server fails to compile against it.
fetch tiltedcore https://codeload.github.com/tiltedphoques/TiltedCore/tar.gz/refs/tags/v0.2.9        TiltedCore-0.2.9
fetch tiltedconnect https://codeload.github.com/TiltedPhoques/TiltedConnect/tar.gz/e6f5759364a64462e717db1153781a7eae26a13e TiltedConnect
fetch hopscotch https://codeload.github.com/Tessil/hopscotch-map/tar.gz/refs/tags/v2.3.1              hopscotch
fetch glm https://codeload.github.com/g-truc/glm/tar.gz/refs/tags/0.9.9.8                            glm
fetch entt https://codeload.github.com/skypjack/entt/tar.gz/refs/tags/v3.10.0                        entt
fetch spdlog https://codeload.github.com/gabime/spdlog/tar.gz/refs/tags/v1.13.0                      spdlog
fetch sol2 https://codeload.github.com/ThePhD/sol2/tar.gz/refs/tags/v3.3.0                           sol2
fetch lua https://codeload.github.com/lua/lua/tar.gz/refs/tags/v5.4.6                                lua
fetch httplib https://codeload.github.com/yhirose/cpp-httplib/tar.gz/refs/tags/v0.14.0               httplib
fetch mimalloc https://codeload.github.com/microsoft/mimalloc/tar.gz/refs/tags/v2.2.4                mimalloc
fetch zlib https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v1.3.1                          zlib

# TiltedCore exposes its headers under a TiltedCore/ prefix in the xmake package.
mkdir -p "${DEPS_DIR}/tcinc"
ln -sfn ../TiltedCore-0.2.9/Code/core/include "${DEPS_DIR}/tcinc/TiltedCore"

# -O0 on purpose: this target exists to be run often, not to be fast at runtime.
OPT="${OPT:--O0}"

if [[ "${MODE}" == "objects" ]]; then
    echo "==> compiling the server (${JOBS} jobs)"
    make -C "${REPO_ROOT}/Tools/rfab/linux-server" OPT="${OPT}" -j"${JOBS}" objects
    exit 0
fi

echo "==> building the in-process server harness (${JOBS} jobs)"
make -C "${REPO_ROOT}/Tools/rfab/linux-server" OPT="${OPT}" -j"${JOBS}" all

echo "==> running the server harness"
"${REPO_ROOT}/build/linux-server/servertests"
