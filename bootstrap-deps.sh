#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DEPS_ROOT="${QUICKREF_DEPS_DIR:-${PROJECT_DIR}/.deps}"
PACKAGE_DIR="${DEPS_ROOT}/packages"
PREFIX="${DEPS_ROOT}/prefix"
MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"

if [[ -f "/usr/include/cmark-gfm-core-extensions.h" ]]; then
    printf '系统已安装 cmark-gfm 开发文件。\n'
    exit 0
fi

mkdir -p "${PACKAGE_DIR}" "${PREFIX}"

if [[ ! -f "${PREFIX}/usr/include/cmark-gfm-core-extensions.h" ||
      ! -f "${PREFIX}/usr/lib/${MULTIARCH}/libcmark-gfm.a" ||
      ! -f "${PREFIX}/usr/lib/${MULTIARCH}/libcmark-gfm-extensions.a" ]]; then
    (
        cd "${PACKAGE_DIR}"
        apt-get download \
            libcmark-gfm-dev \
            libcmark-gfm-extensions-dev \
            libcmark-gfm0.29.0.gfm.3 \
            libcmark-gfm-extensions0.29.0.gfm.3
    )

    for package in "${PACKAGE_DIR}"/*.deb; do
        dpkg-deb -x "${package}" "${PREFIX}"
    done
fi

printf '本地开发依赖已准备：%s\n' "${PREFIX}"
