#!/usr/bin/env bash
# Запуск sheremetevo-app в контейнере: проверяет смонтированные конфиги и
# модель, чтобы ошибка была понятна сразу, а не из глубины DeepStream.
set -euo pipefail

CONFIG="${1:-/app/configs/app.ini}"
MODELS_DIR="/app/models"

fail() {
    echo "[entrypoint] $*" >&2
    exit 1
}

# Значение ключа из INI-файла (первое вхождение, без пробелов по краям)
ini_value() {
    sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*//p" "$1" | head -n1 | sed 's/[[:space:]]*$//'
}

# Относительный путь считается от папки файла, где он записан
resolve() {
    case "$2" in
        /*) echo "$2" ;;
        *) echo "$(dirname "$1")/$2" ;;
    esac
}

[ -f "${CONFIG}" ] || fail "config ${CONFIG} not found: mount configs/ to /app/configs"

nvinfer_value="$(ini_value "${CONFIG}" nvinfer-config)"
[ -n "${nvinfer_value}" ] || fail "nvinfer-config is not set in ${CONFIG}"
NVINFER_CONFIG="$(resolve "${CONFIG}" "${nvinfer_value}")"
[ -f "${NVINFER_CONFIG}" ] || fail "nvinfer config ${NVINFER_CONFIG} not found"

custom_lib="$(ini_value "${NVINFER_CONFIG}" custom-lib-path)"
if [ -n "${custom_lib}" ] && [ ! -f "$(resolve "${NVINFER_CONFIG}" "${custom_lib}")" ]; then
    fail "custom-lib-path ${custom_lib} not found; in this image it is" \
         "/opt/DeepStream-Yolo/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so"
fi

onnx="$(ini_value "${NVINFER_CONFIG}" onnx-file)"
engine="$(ini_value "${NVINFER_CONFIG}" model-engine-file)"
engine_path=""
[ -n "${engine}" ] && engine_path="$(resolve "${NVINFER_CONFIG}" "${engine}")"

if [ -z "${engine_path}" ] || [ ! -f "${engine_path}" ]; then
    [ -n "${onnx}" ] && [ -f "$(resolve "${NVINFER_CONFIG}" "${onnx}")" ] \
        || fail "neither engine ${engine:-<not set>} nor onnx ${onnx:-<not set>} found: mount models/ to ${MODELS_DIR}"
    echo "[entrypoint] TensorRT engine ${engine_path:-<not set>} not found," \
         "it will be built from ${onnx}; the first start takes several minutes."
fi

# Построенный engine сохраняется в текущую папку, поэтому работаем из
# смонтированной models/: при следующих запусках сборка не повторяется.
[ -w "${MODELS_DIR}" ] || fail "${MODELS_DIR} is not writable: the TensorRT engine cache cannot be saved"
cd "${MODELS_DIR}"

# Кэш реестра GStreamer помнит плагины, которые не загрузились (например,
# плагины DeepStream при запуске без GPU), и больше их не пробует. Сбрасываем
# его на каждом старте: пересканирование занимает несколько секунд.
rm -rf "${HOME:-/root}/.cache/gstreamer-1.0"

exec /app/sheremetevo-app "${CONFIG}"
