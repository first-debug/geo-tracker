#!/usr/bin/env bash
# Печатает Debian-пакеты, которым принадлежат разделяемые библиотеки,
# нужные переданным бинарникам. Библиотеки DeepStream из /opt и libcuda
# из драйвера не принадлежат пакетам и пропускаются.
set -euo pipefail

for bin in "$@"; do
    ldd "${bin}"
done | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' | sort -u | while read -r lib; do
    # В Ubuntu с объединённым /usr dpkg знает файл под одним из путей
    dpkg -S "${lib}" 2>/dev/null \
        || dpkg -S "$(readlink -f "${lib}")" 2>/dev/null \
        || dpkg -S "/usr${lib}" 2>/dev/null \
        || true
done | cut -d: -f1 | sort -u
