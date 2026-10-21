# Модель YOLO26

Детектор — YOLO26m (Ultralytics), предобученный на COCO. Используется только класс
4 — `airplane`, остальные отфильтрованы в конфиге `nvinfer`. Модель запускается через
TensorRT внутри `nvinfer`, разбор выхода сети делает библиотека из проекта
[DeepStream-Yolo](https://github.com/marcoslucianops/DeepStream-Yolo).

Цепочка файлов:

```
models/yolo26m.pt ──export_yolo26.py──► models/yolo26m.onnx + labels.txt ──make engine / entrypoint.sh──► *.engine
```

В git хранится только `yolo26m.pt` и скрипт экспорта. ONNX, `labels.txt` и engine
создаются на месте: engine привязан к конкретной видеокарте и версии TensorRT.

## 1. Экспорт в ONNX

**Важно:** ONNX нужно делать скриптом `models/export_yolo26.py`, а не штатным
`yolo export` Ultralytics. Штатный экспорт даёт выход `[batch, 84, 8400]`, который
парсер DeepStream-Yolo не понимает, и детекций будет ноль. Скрипт даёт выход
`[batch, N, 6]` (x1, y1, x2, y2, уверенность, класс).

`models/export_yolo26.py` — копия скрипта из DeepStream-Yolo с исправлениями под
ultralytics 8.4.x (`m.end2end = True` перед `fuse()`, `dynamo=False` в
`torch.onnx.export`). С оригиналом на 8.4.x экспорт ломается.

Экспорт делается в любом окружении Python с `ultralytics` 8.4.x, `torch`, `onnx` и
`onnxslim`; GPU не нужен:

```
cd models
pip install ultralytics onnx onnxslim
python3 export_yolo26.py -w yolo26m.pt --dynamic --simplify
```

| Параметр | Описание |
| :--- | :--- |
| `-w` | Веса `.pt`, обязательно. ONNX сохраняется рядом с тем же именем |
| `-s H [W]` | Размер входа сети, по умолчанию 640 |
| `--dynamic` | Динамический размер пачки: один ONNX подходит для любого числа камер |
| `--batch N` | Статический размер пачки вместо `--dynamic` — равен числу камер |
| `--opset` | Версия opset ONNX, по умолчанию 17 |
| `--simplify` | Упростить граф (onnxslim) |

`labels.txt` создаётся в текущей папке, поэтому скрипт запускается из `models/`.

## 2. Парсер DeepStream-Yolo

`nvinfer` нужна библиотека `libnvdsinfer_custom_impl_Yolo.so`: она разбирает выход сети
и строит engine. В Docker-образе проекта она уже собрана и лежит в
`/opt/DeepStream-Yolo/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so`.

Собрать вручную внутри контейнера DeepStream:

```
git clone https://github.com/marcoslucianops/DeepStream-Yolo.git
cd DeepStream-Yolo
nvcc --version                     # узнать версию CUDA в контейнере
CUDA_VER=<версия, например 12.8> make -C nvdsinfer_custom_impl_Yolo clean all
```

Путь к получившейся библиотеке указывается в `custom-lib-path` конфига `nvinfer`.

## 3. TensorRT engine

Engine строится отдельным шагом до запуска приложения скриптом `tools/build_engine.sh`.
Скрипт запускает короткий тестовый конвейер `nvinfer` с тем же конфигом, что и у
приложения, поэтому engine получается таким же, как если бы его строил сам `nvinfer`
при старте (через библиотеку DeepStream-Yolo, с динамическим или статическим batch).
Готовый файл кладётся по пути из `model-engine-file`, имя может быть любым.

Перед сборкой и при каждом запуске скрипт проверяет, что на месте ONNX, `labels.txt`
(число строк равно `num-detected-classes`), `custom-lib-path` и, для INT8, файл
калибровки. Иначе он сразу завершается с понятной ошибкой.

Внутри контейнера DeepStream, из корня репозитория:

```
make engine              # построить, если нужно (несколько минут)
make engine-rebuild      # построить заново в любом случае
make engine NVINFER_CONFIG=configs/другой_конфиг.txt
```

В Docker то же самое делает `entrypoint.sh` при каждом старте контейнера
(см. [deploy.md](deploy.md#tensorrt-engine)). Собирать engine в Dockerfile нельзя: при
`docker build` нет GPU, а engine привязан к видеокарте и версии TensorRT.

Рядом с engine сохраняется `<engine>.stamp`:

```
onnx-sha256=…
tensorrt=libnvinfer.so.10.…
gpu=NVIDIA GeForce RTX …, 8.9
batch-size=4
network-mode=0
int8-calib-file=
```

При запуске скрипт сравнивает его с текущими значениями и пересобирает engine, если
поменялись:

- ONNX (по хэшу содержимого, имя файла может остаться прежним);
- видеокарта или версия TensorRT (новый образ DeepStream);
- `batch-size` (число камер) — при статическом ONNX нужен и новый экспорт;
- `network-mode` (FP32/FP16/INT8) или файл калибровки INT8.

Если всё совпадает, проверка занимает около секунды (хэш ONNX). Engine без `.stamp`
(например, построенный приложением раньше) проверить нельзя, поэтому он один раз
строится заново. Старый engine заменяется только после успешной сборки.

Изменения, которые скрипт не видит, — например, новая версия парсера DeepStream-Yolo
(`DSYOLO_REF`) — требуют `make engine-rebuild` или удаления engine.

Если engine всё-таки не нашёлся (приложение запущено без `make engine`), `nvinfer`
построит его сам при старте и библиотека DeepStream-Yolo сохранит его в текущую папку
под именем `model_b<batch>_gpu<id>_<fp32|fp16|int8>.engine`. Такой файл нужно вручную
перенести по пути из `model-engine-file`, иначе сборка будет повторяться при каждом
запуске. Поэтому удобнее всегда запускать `make engine` перед приложением.

## Параметры nvinfer

Полный перечень ключей — в [configuration.md](configuration.md#config_infer_primary_yolotxt).
Для скорости в первую очередь стоит попробовать `network-mode=2` (FP16) и, если
нужно, `interval=1` (детекция через кадр, между ними объекты ведёт трекер).
