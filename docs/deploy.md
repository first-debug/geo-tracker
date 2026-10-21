# Развёртывание в Docker

Приложение запускается в контейнере на базе образа NVIDIA DeepStream 9.0.
Образ собирается из исходников этого репозитория; конфиги и модель в образ не
входят и монтируются с хоста, поэтому смена камеры, калибровки или модели не
требует пересборки.

## Что нужно на сервере

- Linux x86_64 с GPU NVIDIA и драйвером 590.48.01 или новее.
- Docker Engine с плагином Compose v2 (`docker compose version`).
- [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html),
  настроенный для Docker (`sudo nvidia-ctk runtime configure --runtime=docker`).
  Проверка: `docker run --rm --gpus all ubuntu nvidia-smi`.
- Доступ в интернет при сборке: скачиваются образ DeepStream с nvcr.io,
  пакеты Ubuntu и исходники DeepStream-Yolo.
- Сетевой доступ к камерам (RTSP), к RTSP-серверу видеовыхода и к получателю UDP.

## Файлы

| Файл | Назначение |
| :--- | :--- |
| `docker/Dockerfile` | Двухстадийная сборка: в первой стадии компилируются приложение и парсер DeepStream-Yolo, во второй остаются только они и нужные для работы библиотеки. |
| `docker/entrypoint.sh` | Проверяет конфиги и модель перед стартом и запускает приложение. |
| `docker/healthcheck.sh` | Docker HEALTHCHECK по heartbeat-файлу приложения. |
| `docker/runtime-deps.sh` | Находит системные пакеты, нужные собранным файлам, чтобы поставить их в итоговый образ. |
| `docker-compose.yml` | Запуск: GPU, сеть хоста, автоперезапуск, тома, ротация логов. |
| `.env.example` | Переменные сборки и запуска; копируется в `.env`. |
| `.dockerignore` | В контекст сборки не попадают конфиги, модели, `build/` и `.git`. |

## Подготовка

На сервере нужна копия репозитория с заполненными `configs/` и `models/`:

```
git clone <адрес репозитория> sheremetevo
cd sheremetevo
cp .env.example .env
```

Конфиги готовятся так же, как для разработки (см. README), с одним отличием:
в образе парсер YOLO лежит по другому пути. В `configs/config_infer_primary_yolo.txt`:

```
custom-lib-path=/opt/DeepStream-Yolo/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so
```

В `models/` должны лежать ONNX-модель и `labels.txt`, на которые ссылается
`config_infer_primary_yolo.txt`. ONNX экспортируется скриптом
`export_yolo26.py` из DeepStream-Yolo (выход `[batch, N, 6]`).

В контейнере папки смонтированы так:

| Хост | Контейнер | Режим |
| :--- | :--- | :--- |
| `./configs` | `/app/configs` | только чтение |
| `./models` | `/app/models` | чтение и запись (кэш TensorRT engine) |

Относительные пути в конфигах (`../models/...`, `calibration/...`) работают
без изменений.

## Запуск

```
docker compose up -d --build
docker compose logs -f
```

При старте в логе видна ошибка репроекции калибровки по каждой камере
(`[CALIB] camera N: ...`), затем раз в 5 секунд статистика по потокам.

Остановка: `docker compose down`. Приложение получает SIGTERM, закрывает
RTSP-сессии и энкодеры и завершается; на это отведено 20 секунд.

Контейнер перезапускается автоматически, если приложение упало, и после
перезагрузки сервера (`restart: unless-stopped`, служба Docker должна быть
включена: `sudo systemctl enable docker`). Отвалившиеся камеры
переподключает само приложение, перезапуск контейнера для этого не нужен.

## Контроль работоспособности

Приложение раз в 2 секунды пишет heartbeat-файл `/tmp/sheremetevo-health`
внутри контейнера:

```
time=1792581415
uptime_sec=3600
last_frame_age_sec=0.1
cameras_receiving=4/4
```

`docker/healthcheck.sh` проверяет его каждые 15 секунд. Контейнер
считается здоровым (`healthy`), если файл обновлялся не позже 15 секунд
назад и хотя бы одна камера дала обработанный кадр за последние
`HEALTH_MAX_FRAME_AGE_SEC` секунд (60 по умолчанию). Одна отвалившаяся
камера контейнер нездоровым не делает: это видно по `cameras_receiving`.
Первые 10 минут после старта неудачные проверки не считаются: в это время
может строиться TensorRT engine.

```
docker compose ps                     # STATUS: healthy / unhealthy
docker inspect --format '{{json .State.Health}}' sheremetevo-app-1
```

Docker сам не перезапускает контейнер со статусом `unhealthy`, поэтому
перезапуском занимается приложение (секция `[health]` в `app.ini`):

| Ключ | По умолчанию | Что происходит |
| :--- | :--- | :--- |
| `heartbeat-file` | `/tmp/sheremetevo-health` | Куда писать heartbeat; пусто — не писать. Должен совпадать с `HEALTH_FILE` в `docker-compose.yml`. |
| `hang-timeout-sec` | `60` | Главный цикл не отвечает дольше (зависание) — процесс завершается с ошибкой, Docker перезапускает контейнер. `0` — не следить. |
| `no-frames-restart-sec` | `300` | Ни одного обработанного кадра со всех камер дольше — штатная остановка с кодом 1 и перезапуск. Помогает, если завис конвейер; если же недоступны сами камеры, приложение будет перезапускаться с этим интервалом. `0` — не перезапускать. |

Перезапуски видны в `docker compose ps` (поле RESTARTS у `docker inspect`)
и в логе по строкам `[HEALTH]`.

## TensorRT engine

Engine зависит от модели GPU, версии TensorRT и размера пачки (`batch-size`,
равен числу камер). Если файла из `model-engine-file` нет, nvinfer строит
engine из ONNX при первом запуске: это занимает несколько минут, в логе
будет строка `TensorRT engine ... not found, it will be built`. Готовый
engine сохраняется в `models/` на хосте, следующие запуски стартуют сразу.

Удалите engine из `models/` и проверьте имя в `model-engine-file`, если:

- поменялась ONNX-модель;
- поменялось число камер (`batch-size`) или точность (`network-mode`);
- сервер получил другую GPU, или обновился образ DeepStream или драйвер.

Engine, построенный на машине разработчика, на сервер не переносится:
он собирается под конкретную GPU.

## Обновление и откат

Образ помечается коммитом, из которого собран:

```
git pull
export APP_VERSION=$(git rev-parse --short HEAD) GIT_SHA=$(git rev-parse HEAD)
docker compose up -d --build
```

Старые образы остаются в Docker (`docker images sheremetevo-app`).
Откат на предыдущую версию без пересборки:

```
APP_VERSION=<старый тег> docker compose up -d
```

## Настройки (`.env`)

| Переменная | По умолчанию | Назначение |
| :--- | :--- | :--- |
| `DS_IMAGE` | `nvcr.io/nvidia/deepstream:9.0-triton-multiarch` | Базовый образ DeepStream. Должен совпадать с образом разработки. |
| `DSYOLO_REF` | `master` | Коммит DeepStream-Yolo. Для воспроизводимой сборки укажите коммит, с которым проверена модель. |
| `APP_VERSION` | `latest` | Тег образа. |
| `APP_CONFIG` | `app.ini` | Конфиг из `configs/`, с которым запускается приложение (например `app.test-video.ini`). |
| `HEALTH_MAX_FRAME_AGE_SEC` | `60` | Сколько секунд без обработанных кадров контейнер ещё считается здоровым. |
| `GST_DEBUG` | `1` | Логи GStreamer: `1` — ошибки, `2` — и предупреждения, `3`+ — для отладки. |
| `TZ` | `Europe/Moscow` | Часовой пояс в логах. |

## Логи

Логи пишет Docker (`docker compose logs`), ротация: 5 файлов по 50 МБ.
Предупреждения v4l2 `Could not probe minimum capture size` при старте
безвредны и при `GST_DEBUG=1` не выводятся.

## Частые проблемы

| Сообщение | Причина и решение |
| :--- | :--- |
| `config /app/configs/app.ini not found` | Не смонтирована `configs/` или в ней нет `app.ini`: запускайте `docker compose` из корня репозитория. |
| `custom-lib-path ... not found` | В `config_infer_primary_yolo.txt` путь к парсеру из среды разработки, нужен путь из раздела «Подготовка». |
| `neither engine ... nor onnx ... found` | В `models/` нет ONNX-модели. |
| `/app/models is not writable` | У папки `models/` на хосте нет прав на запись. |
| `no such element factory "nvstreammux"` / `"nvinfer"` | Плагины DeepStream не загрузились. В образах до исправления кэш реестра GStreamer оставался со сборки без GPU — пересоберите образ. Если ошибка осталась, контейнер не видит GPU: проверьте `docker compose run --rm --entrypoint nvidia-smi app`, затем `docker compose run --rm --entrypoint gst-inspect-1.0 app nvinfer` — в выводе будет причина, по которой плагин не загрузился. |
| `could not select device driver "nvidia"` | Не установлен или не настроен NVIDIA Container Toolkit. |
| Ошибки `nvv4l2decoder` / `nvv4l2h264enc` | Контейнеру не выдана capability `video`: проверьте, что запуск идёт через `docker-compose.yml`. |
| `Cannot open UDP connection` | Неверный `[udp] host` в `app.ini`. |

## Проверка образа без GPU

При сборке итоговый образ проверяется: все библиотеки приложения найдены и
элементы GStreamer `rtspsrc`, `rtph264depay`, `h264parse`, `rtspclientsink`
установлены. Проверка работы с GPU возможна только запуском на сервере.
