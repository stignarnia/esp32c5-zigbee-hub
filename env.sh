# Source this file (zsh or bash) to get the project-local ESP-IDF toolchain:
#   source ./env.sh
# Everything lives in ./.esp — delete that folder to remove the toolchain entirely.

if [ -n "${ZSH_VERSION:-}" ]; then
  _proj_root="${${(%):-%x}:A:h}"
else
  _proj_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi

export ESP_ROOT="$_proj_root/.esp"
export IDF_PATH="$ESP_ROOT/esp-idf"
export IDF_TOOLS_PATH="$ESP_ROOT/tools"
export IDF_PYTHON_ENV_PATH="$ESP_ROOT/venv"
export IDF_COMPONENT_CACHE_PATH="$ESP_ROOT/components-cache"
export UV_PYTHON_INSTALL_DIR="$ESP_ROOT/python"
export UV_CACHE_DIR="$ESP_ROOT/uv-cache"
export PIP_CACHE_DIR="$ESP_ROOT/pip-cache"
# Serial port: keep an ESPPORT you set yourself, else the first CH34x USB-UART or
# native Espressif USB port found. Left unset when no board is plugged in
# (idf.py refuses to build if ESPPORT names a port it can't read).
if [ -z "${ESPPORT:-}" ]; then
  _port="$(ls -1 /dev/serial/by-id/ 2>/dev/null | grep -m1 -E '^usb-(1a86|Espressif)')"
  [ -n "$_port" ] && export ESPPORT="/dev/serial/by-id/$_port"
  unset _port
fi

if [ "${1:-}" != "--vars-only" ]; then
  if [ -f "$IDF_PATH/export.sh" ]; then
    . "$IDF_PATH/export.sh" >/dev/null
  else
    echo "env.sh: ESP-IDF not found at $IDF_PATH" >&2
  fi
fi

unset _proj_root
