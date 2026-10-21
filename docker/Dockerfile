# syntax=docker/dockerfile:1
#
# Образ sheremetevo-app. Сборка из корня репозитория:
#   docker compose build
# или
#   docker build -f docker/Dockerfile -t sheremetevo-app .
#
# Обе стадии строятся на одном образе DeepStream: в итоговый образ не попадают
# компиляторы и -dev пакеты, а библиотеки DeepStream/TensorRT/CUDA совпадают
# с теми, с которыми собиралось приложение.

ARG DS_IMAGE=nvcr.io/nvidia/deepstream:9.0-triton-multiarch

########################################################################
# Сборка приложения и парсера DeepStream-Yolo
########################################################################
FROM ${DS_IMAGE} AS build

ARG DEBIAN_FRONTEND=noninteractive
# Коммит или тег https://github.com/marcoslucianops/DeepStream-Yolo.
# Для воспроизводимой сборки укажите конкретный коммит, с которым проверена модель.
ARG DSYOLO_REF=master

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        git \
        pkg-config \
        libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev \
        libopencv-dev \
        libprotobuf-dev \
        protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*

# libcuda.so приходит из драйвера хоста только при запуске контейнера с GPU,
# поэтому при сборке компоновщик берёт заглушку из CUDA toolkit.
ENV LIBRARY_PATH=/usr/local/cuda/lib64/stubs

# Парсер выходов YOLO для nvinfer (custom-lib-path в config_infer_primary_yolo.txt)
RUN git clone https://github.com/marcoslucianops/DeepStream-Yolo.git /opt/DeepStream-Yolo \
    && cd /opt/DeepStream-Yolo \
    && git checkout "${DSYOLO_REF}" \
    && CUDA_VER="$(ls -d /usr/local/cuda-* | head -n1 | sed 's|.*/cuda-||')" \
    && echo "Building DeepStream-Yolo ${DSYOLO_REF} with CUDA ${CUDA_VER}" \
    && make -C nvdsinfer_custom_impl_Yolo -j"$(nproc)" CUDA_VER="${CUDA_VER}"

WORKDIR /src
COPY . .
RUN make clean && make -j"$(nproc)"

# Список системных пакетов, которые нужны собранным файлам во время работы
COPY docker/runtime-deps.sh /usr/local/bin/runtime-deps.sh
RUN bash /usr/local/bin/runtime-deps.sh \
        build/sheremetevo-app \
        /opt/DeepStream-Yolo/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so \
        > /runtime-packages.txt \
    && echo "Runtime packages:" && cat /runtime-packages.txt

########################################################################
# Итоговый образ
########################################################################
FROM ${DS_IMAGE} AS runtime

ARG DEBIAN_FRONTEND=noninteractive
ARG GIT_SHA=unknown
LABEL org.opencontainers.image.title="sheremetevo-app" \
      org.opencontainers.image.revision="${GIT_SHA}"

# rtspsrc, rtph264depay — plugins-good; h264parse — plugins-bad;
# rtspclientsink (видеовыход) — gstreamer1.0-rtsp.
COPY --from=build /runtime-packages.txt /tmp/runtime-packages.txt
RUN apt-get update \
    && xargs -a /tmp/runtime-packages.txt apt-get install -y --no-install-recommends \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-rtsp \
    && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages.txt

COPY --from=build /opt/DeepStream-Yolo/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so \
                  /opt/DeepStream-Yolo/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so
COPY --from=build /src/build/sheremetevo-app /app/sheremetevo-app
COPY docker/entrypoint.sh /app/entrypoint.sh
COPY docker/healthcheck.sh /app/healthcheck.sh
COPY tools/build_engine.sh /app/build_engine.sh
RUN chmod +x /app/entrypoint.sh /app/healthcheck.sh /app/build_engine.sh

# Проверка образа без GPU: все библиотеки найдены (кроме libcuda из драйвера)
# и нужные элементы GStreamer есть. gst-inspect при этом создаёт кэш реестра
# плагинов, в котором плагины DeepStream без GPU помечены как не загрузившиеся;
# такой кэш нельзя оставлять в образе, иначе nvinfer и nvstreammux не найдутся.
RUN missing="$(ldd /app/sheremetevo-app | grep 'not found' | grep -v 'libcuda.so' || true)" \
    && if [ -n "${missing}" ]; then echo "Missing libraries:"; echo "${missing}"; exit 1; fi \
    && for el in rtspsrc rtph264depay h264parse rtspclientsink; do \
           gst-inspect-1.0 "${el}" > /dev/null || { echo "GStreamer element ${el} not found"; exit 1; }; \
       done \
    && rm -rf /root/.cache/gstreamer-1.0

# Видеокодеки nvv4l2decoder/nvv4l2h264enc требуют capability video
ENV NVIDIA_DRIVER_CAPABILITIES=compute,utility,video \
    GST_DEBUG=1

# configs/ монтируется только на чтение, models/ — на запись (кэш TensorRT engine).
VOLUME ["/app/models"]

# Приложение пишет heartbeat-файл ([health] в app.ini). start-period длинный:
# при первом запуске entrypoint.sh строит TensorRT engine несколько минут.
HEALTHCHECK --interval=15s --timeout=5s --start-period=10m --retries=3 \
    CMD ["/app/healthcheck.sh"]
WORKDIR /app
ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["/app/configs/app.ini"]
