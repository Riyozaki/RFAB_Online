#!/usr/bin/env bash
#
# Fetch the platform-independent dependencies and run the fast core test suite.
#
# This is the feedback loop the project was missing: protocol/encoding and coop-core logic are
# compiled and executed locally in ~2 minutes cold / seconds incrementally, instead of being
# validated only by a 13-19 minute Windows release pipeline (or by playing the game).
#
#   ./Tools/rfab/run-core-tests.sh             # fetch (once), build, run
#   ./Tools/rfab/run-core-tests.sh --clean     # rebuild from scratch
#
# Requirements: bash, curl, tar, make, g++ (C++20). No submodules, no xmake, no network beyond
# codeload.github.com.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPS_DIR="${REPO_ROOT}/build/deps"
JOBS="${JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 2 )}"

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf "${REPO_ROOT}/build/linux-core"
fi

fetch() {
    local name="$1" url="$2" expect_dir="$3"
    if [[ -d "${DEPS_DIR}/${expect_dir}" ]]; then
        return 0
    fi
    echo "==> fetching ${name}"
    mkdir -p "${DEPS_DIR}"
    curl -fsSL "${url}" -o "${DEPS_DIR}/${name}.tar.gz"
    tar xzf "${DEPS_DIR}/${name}.tar.gz" -C "${DEPS_DIR}"
    rm -f "${DEPS_DIR}/${name}.tar.gz"
    [[ -d "${DEPS_DIR}/${expect_dir}" ]] || { echo "!! ${expect_dir} missing after extracting ${name}" >&2; exit 1; }
}

fetch tiltedcore    https://codeload.github.com/tiltedphoques/TiltedCore/tar.gz/refs/tags/v0.2.9   TiltedCore-0.2.9
fetch hopscotch-map https://codeload.github.com/Tessil/hopscotch-map/tar.gz/refs/tags/v2.3.1       hopscotch-map-2.3.1
fetch glm           https://codeload.github.com/g-truc/glm/tar.gz/refs/tags/0.9.9.8                glm-0.9.9.8
fetch catch2        https://codeload.github.com/catchorg/Catch2/tar.gz/refs/tags/v2.13.10          Catch2-2.13.10
fetch mimalloc      https://codeload.github.com/microsoft/mimalloc/tar.gz/refs/tags/v2.2.4         mimalloc-2.2.4

# The xmake tiltedcore package exposes its headers under a TiltedCore/ prefix; mirror that layout.
mkdir -p "${DEPS_DIR}/tcinc"
ln -sfn "${DEPS_DIR}/TiltedCore-0.2.9/Code/core/include" "${DEPS_DIR}/tcinc/TiltedCore"

echo "==> building core tests (-j${JOBS})"
make -C "${REPO_ROOT}/Tools/rfab/linux-core" -j"${JOBS}" all

echo "==> running core tests"
exec make -C "${REPO_ROOT}/Tools/rfab/linux-core" run
