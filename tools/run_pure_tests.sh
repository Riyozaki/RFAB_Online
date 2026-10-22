#!/usr/bin/env bash
# Builds and runs the dependency-free gameplay tests.
#
# These need only g++/clang++ and C++17 - no xmake, no EnTT, no Skyrim. They run
# in CI, on a Linux server, and in the development sandbox, which makes them the
# only part of this repository that can be verified without a Windows toolchain.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-g++}"
out="${root}/.work/pure_tests"

mkdir -p "${root}/.work"

"${cxx}" -std=c++17 -O2 -Wall -Wextra -Werror \
    -I "${root}/Code/common" \
    "${root}/Code/tests/pure/pure_tests.cpp" \
    "${root}/Code/common/Gameplay/"*.cpp \
    -o "${out}"

"${out}" "$@"
