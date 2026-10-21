#!/usr/bin/env bash
# Docker HEALTHCHECK: контейнер здоров, если приложение недавно обновляло
# heartbeat-файл (главный цикл жив) и недавно обработало кадр хотя бы одной
# камеры. Файл пишет приложение, см. секцию [health] в app.ini.
set -u

HEALTH_FILE="${HEALTH_FILE:-/tmp/sheremetevo-health}"
# heartbeat пишется раз в 2 с
MAX_HEARTBEAT_AGE_SEC="${HEALTH_MAX_HEARTBEAT_AGE_SEC:-15}"
MAX_FRAME_AGE_SEC="${HEALTH_MAX_FRAME_AGE_SEC:-60}"

if [ ! -f "${HEALTH_FILE}" ]; then
    echo "no heartbeat file ${HEALTH_FILE} (application is starting or [health] heartbeat-file differs)"
    exit 1
fi

heartbeat_age=$(( $(date +%s) - $(stat -c %Y "${HEALTH_FILE}") ))
if [ "${heartbeat_age}" -gt "${MAX_HEARTBEAT_AGE_SEC}" ]; then
    echo "heartbeat is ${heartbeat_age} s old: main loop is not responding"
    exit 1
fi

# В выводе healthcheck (docker inspect) видно состояние камер
cat "${HEALTH_FILE}"

frame_age="$(sed -n 's/^last_frame_age_sec=//p' "${HEALTH_FILE}")"
case "${frame_age}" in
    ""|-*)
        echo "no frames processed yet"
        exit 1
        ;;
esac
if ! awk -v age="${frame_age}" -v max="${MAX_FRAME_AGE_SEC}" 'BEGIN { exit !(age <= max) }'; then
    echo "no frames processed for ${frame_age} s"
    exit 1
fi
exit 0
