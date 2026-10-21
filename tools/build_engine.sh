#!/usr/bin/env bash
# Сборка TensorRT engine для nvinfer до запуска приложения.
#
#   tools/build_engine.sh [--force] [config_infer_primary_yolo.txt]
#
# Engine строит сам nvinfer (через custom-lib-path из DeepStream-Yolo) на
# коротком тестовом конвейере с тем же конфигом, что и у приложения, поэтому
# результат такой же, как при сборке во время старта приложения. Готовый файл
# кладётся по пути из model-engine-file.
#
# Рядом сохраняется <engine>.stamp: хэш ONNX, версия TensorRT, GPU и параметры
# сборки. Если что-то из этого поменялось или отметки нет, engine строится
# заново, иначе скрипт сразу завершается. --force — собрать в любом случае.
#
# Нужны GPU и DeepStream: запускать в контейнере DeepStream.
set -euo pipefail

FORCE=0
CONFIG=configs/config_infer_primary_yolo.txt
while [ $# -gt 0 ]; do
    case "$1" in
        -f|--force) FORCE=1 ;;
        -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) CONFIG="$1" ;;
    esac
    shift
done

log() {
    echo "[engine] $*" >&2
}

fail() {
    log "$*"
    exit 1
}

# Значение ключа из INI-файла (первое вхождение, без пробелов по краям)
ini_value() {
    sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*//p" "$1" | head -n1 | sed 's/[[:space:]]*$//'
}

# Абсолютный путь; относительный считается от папки конфига nvinfer
resolve() {
    [ -n "$1" ] || return 0
    case "$1" in
        /*) realpath -m "$1" ;;
        *) realpath -m "${CONFIG_DIR}/$1" ;;
    esac
}

tensorrt_version() {
    local lib
    lib="$(ldconfig -p 2>/dev/null | awk '/libnvinfer\.so\.[0-9]/ { print $NF; exit }')"
    if [ -n "${lib}" ]; then
        basename "$(readlink -f "${lib}")"
    else
        echo unknown
    fi
}

gpu_name() {
    nvidia-smi -i "${GPU_ID}" --query-gpu=name,compute_cap --format=csv,noheader 2>/dev/null \
        || nvidia-smi -i "${GPU_ID}" --query-gpu=name --format=csv,noheader 2>/dev/null \
        || echo unknown
}

[ -f "${CONFIG}" ] || fail "nvinfer config ${CONFIG} not found"
CONFIG="$(realpath "${CONFIG}")"
CONFIG_DIR="$(dirname "${CONFIG}")"

ONNX="$(resolve "$(ini_value "${CONFIG}" onnx-file)")"
ENGINE="$(resolve "$(ini_value "${CONFIG}" model-engine-file)")"
LABELS="$(resolve "$(ini_value "${CONFIG}" labelfile-path)")"
CUSTOM_LIB="$(resolve "$(ini_value "${CONFIG}" custom-lib-path)")"
CALIB="$(resolve "$(ini_value "${CONFIG}" int8-calib-file)")"
BATCH="$(ini_value "${CONFIG}" batch-size)"
GPU_ID="$(ini_value "${CONFIG}" gpu-id)"
MODE="$(ini_value "${CONFIG}" network-mode)"
BATCH="${BATCH:-1}"
GPU_ID="${GPU_ID:-0}"
MODE="${MODE:-0}"

[ -n "${ONNX}" ] || fail "onnx-file is not set in ${CONFIG}"
[ -f "${ONNX}" ] || fail "onnx ${ONNX} not found"
[ -n "${ENGINE}" ] || fail "model-engine-file is not set in ${CONFIG}"
[ -z "${CUSTOM_LIB}" ] || [ -f "${CUSTOM_LIB}" ] || fail "custom-lib-path ${CUSTOM_LIB} not found"

ENGINE_DIR="$(dirname "${ENGINE}")"
STAMP="${ENGINE}.stamp"
mkdir -p "${ENGINE_DIR}"
[ -w "${ENGINE_DIR}" ] || fail "${ENGINE_DIR} is not writable"

# Два процесса с общей папкой моделей (например, два контейнера) не должны
# строить один engine одновременно: второй дождётся первого и возьмёт готовый.
if command -v flock > /dev/null; then
    exec 9> "${ENGINE_DIR}/.$(basename "${ENGINE}").lock"
    if ! flock -n 9; then
        log "another build of ${ENGINE} is running, waiting for it"
        flock 9
    fi
fi

log "computing stamp for ${ONNX}"
expected="$(cat <<EOF
onnx-sha256=$(sha256sum "${ONNX}" | cut -d' ' -f1)
tensorrt=$(tensorrt_version)
gpu=$(gpu_name)
batch-size=${BATCH}
network-mode=${MODE}
int8-calib-file=${CALIB}
EOF
)"

if [ "${FORCE}" = 1 ]; then
    log "rebuild requested"
elif [ ! -f "${ENGINE}" ]; then
    log "${ENGINE} not found"
elif [ ! -f "${STAMP}" ]; then
    log "${STAMP} not found, the engine cannot be checked"
elif [ "$(cat "${STAMP}")" != "${expected}" ]; then
    log "the engine is outdated:"
    diff <(cat "${STAMP}") <(echo "${expected}") | sed -n 's/^[<>]/  &/p' >&2 || true
else
    log "${ENGINE} is up to date"
    exit 0
fi

command -v gst-launch-1.0 > /dev/null || fail "gst-launch-1.0 not found: run inside the DeepStream container"

# Временная папка рядом с engine: готовый файл переносится в model-engine-file
# переименованием, и старый engine заменяется только успешной сборкой.
TMP="$(mktemp -d "${ENGINE_DIR}/.engine-build.XXXXXX")"
trap 'rm -rf "${TMP}"' EXIT

# Копия конфига с абсолютными путями. model-engine-file указывает на
# несуществующий файл, иначе nvinfer загрузил бы старый engine вместо сборки.
awk -v onnx="${ONNX}" -v engine="${TMP}/build.engine" -v labels="${LABELS}" \
    -v lib="${CUSTOM_LIB}" -v calib="${CALIB}" '
    function key(line) { sub(/^[[:space:]]*/, "", line); sub(/[[:space:]]*=.*/, "", line); return line }
    /^[[:space:]]*[a-z0-9-]+[[:space:]]*=/ {
        k = key($0)
        if (k == "model-engine-file") next
        if (k == "onnx-file") { print k "=" onnx; next }
        if (k == "labelfile-path" && labels != "") { print k "=" labels; next }
        if (k == "custom-lib-path" && lib != "") { print k "=" lib; next }
        if (k == "int8-calib-file" && calib != "") { print k "=" calib; next }
    }
    { print }
    /^\[property\]/ { print "model-engine-file=" engine }
' "${CONFIG}" > "${TMP}/config_infer.txt"

log "building TensorRT engine: batch-size=${BATCH}, network-mode=${MODE}, gpu-id=${GPU_ID}; this takes several minutes"
touch "${TMP}/.start"
SECONDS=0
# Библиотека DeepStream-Yolo сохраняет engine в текущую папку
(
    cd "${TMP}"
    gst-launch-1.0 \
        videotestsrc num-buffers=1 \
        ! "video/x-raw,width=640,height=640,format=I420" \
        ! nvvideoconvert gpu-id="${GPU_ID}" \
        ! "video/x-raw(memory:NVMM),format=NV12" \
        ! mux.sink_0 \
        nvstreammux name=mux gpu-id="${GPU_ID}" batch-size="${BATCH}" \
            width=640 height=640 batched-push-timeout=40000 \
        ! nvinfer config-file-path="${TMP}/config_infer.txt" \
        ! fakesink
) || fail "nvinfer failed to build the engine, see the log above"

# Обычно файл в TMP; на случай другой версии DeepStream-Yolo ищем и рядом с ONNX
built="$(find "${TMP}" "$(dirname "${ONNX}")" -maxdepth 1 -type f -name '*.engine' \
    -newer "${TMP}/.start" -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -n1 | cut -d' ' -f2-)"
[ -n "${built}" ] || fail "the pipeline finished, but no new .engine file was found"

mv -f "${built}" "${ENGINE}"
echo "${expected}" > "${STAMP}"
log "${ENGINE} built in ${SECONDS} s"
