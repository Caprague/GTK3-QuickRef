#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DEPS_PREFIX="${PROJECT_DIR}/.deps/prefix"
MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"

if [[ ! -f "/usr/include/cmark-gfm-core-extensions.h" &&
      ! -f "${DEPS_PREFIX}/usr/include/cmark-gfm-core-extensions.h" ]] ||
   [[ ! -f "${DEPS_PREFIX}/usr/lib/${MULTIARCH}/libcmark-gfm.a" ]]; then
    "${PROJECT_DIR}/bootstrap-deps.sh"
fi

cmake -S "${PROJECT_DIR}" -B "${PROJECT_DIR}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${PROJECT_DIR}/build" --parallel
