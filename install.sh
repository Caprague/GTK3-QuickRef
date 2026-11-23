#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_BIN="${PROJECT_DIR}/build/gtk-quickref"
SOURCE_PAGES="${PROJECT_DIR}/pages"
SOURCE_STYLE="${PROJECT_DIR}/style.css"
SOURCE_SETTINGS="${PROJECT_DIR}/settings.conf"

BIN_DIR="${HOME}/.local/bin"
CONFIG_DIR="${XDG_CONFIG_HOME:-${HOME}/.config}/gtk-quickref"
PAGES_DIR="${CONFIG_DIR}/pages"
APPLICATION_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/applications"

if [[ ! -x "${SOURCE_BIN}" ]]; then
    printf '未找到可执行文件：%s\n请先运行 build.sh。\n' "${SOURCE_BIN}" >&2
    exit 1
fi

install -d "${BIN_DIR}" "${PAGES_DIR}" "${APPLICATION_DIR}"
install -m 0755 "${SOURCE_BIN}" "${BIN_DIR}/gtk-quickref"
install -m 0644 "${PROJECT_DIR}/gtk-quickref.desktop" \
    "${APPLICATION_DIR}/gtk-quickref.desktop"

if [[ ! -e "${CONFIG_DIR}/style.css" ]]; then
    install -m 0644 "${SOURCE_STYLE}" "${CONFIG_DIR}/style.css"
fi

if [[ ! -e "${CONFIG_DIR}/settings.conf" ]]; then
    install -m 0644 "${SOURCE_SETTINGS}" "${CONFIG_DIR}/settings.conf"
fi

if [[ ! -e "${CONFIG_DIR}/.examples-installed" ]]; then
    while IFS= read -r -d '' source_page; do
        destination_page="${PAGES_DIR}/$(basename -- "${source_page}")"
        if [[ ! -e "${destination_page}" ]]; then
            install -m 0644 "${source_page}" "${destination_page}"
        fi
    done < <(
        find "${SOURCE_PAGES}" -maxdepth 1 -type f \
            \( -name '*.txt' -o -name '*.md' -o -name '*.markdown' \) \
            -print0
    )
    touch "${CONFIG_DIR}/.examples-installed"
fi

CUSTOM_PATH="/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/custom-quickref/"
CURRENT_PATHS="$(gsettings get org.gnome.settings-daemon.plugins.media-keys custom-keybindings)"

if [[ "${CURRENT_PATHS}" != *"${CUSTOM_PATH}"* ]]; then
    if [[ "${CURRENT_PATHS}" == "@as []" || "${CURRENT_PATHS}" == "[]" ]]; then
        NEW_PATHS="['${CUSTOM_PATH}']"
    else
        NEW_PATHS="${CURRENT_PATHS%]}, '${CUSTOM_PATH}']"
    fi
    gsettings set \
        org.gnome.settings-daemon.plugins.media-keys custom-keybindings \
        "${NEW_PATHS}"
fi

KEY_SCHEMA="org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:${CUSTOM_PATH}"
gsettings set "${KEY_SCHEMA}" name '快捷操作提示'
gsettings set "${KEY_SCHEMA}" command "${BIN_DIR}/gtk-quickref"
gsettings set "${KEY_SCHEMA}" binding '<Super><Shift>slash'

printf '已安装：%s\n' "${BIN_DIR}/gtk-quickref"
printf '文档目录：%s\n' "${PAGES_DIR}"
printf '快捷键：Super + Shift + /\n'
