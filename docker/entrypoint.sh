#!/usr/bin/env bash
# Запуск sheremetevo-app в контейнере: проверяет смонтированные конфиги и
# модель, чтобы ошибка была понятна сразу, а не из глубины DeepStream, и
# заранее строит TensorRT engine (tools/build_engine.sh).
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

[ -w "${MODELS_DIR}" ] || fail "${MODELS_DIR} is not writable: the TensorRT engine cannot be saved"

# Кэш реестра GStreamer помнит плагины, которые не загрузились (например,
# плагины DeepStream при запуске без GPU), и больше их не пробует. Сбрасываем
# его на каждом старте, до сборки engine: пересканирование занимает несколько секунд.
rm -rf "${HOME:-/root}/.cache/gstreamer-1.0"

# Engine строится до старта приложения, если его нет или поменялись ONNX, GPU,
# версия TensorRT или параметры сборки; иначе проверка занимает секунду.
/app/build_engine.sh "${NVINFER_CONFIG}" \
    || fail "TensorRT engine was not built, see [engine] messages above"

# Если nvinfer всё же решит пересобрать engine (например, файл повреждён),
# DeepStream-Yolo сохранит его в текущую папку: пусть это будет models/.
cd "${MODELS_DIR}"

exec /app/sheremetevo-app "${CONFIG}"
