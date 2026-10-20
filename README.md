# ГеоТрекер ВС

## [Паспорт проекта](project_passport.md)

## [Техническое задание](https://docs.google.com/document/d/1ofnDw1J2EeLFSGgIVBBEvvBLIBjM1cyu/edit?usp=sharing&ouid=110476095463723163508&rtpof=true&sd=true)

## Запуск

Сборка и запуск — только в Docker-контейнере с DeepStream.

```
make
./build/sheremetevo-app configs/app.ini
```

Все настройки — в [configs/app.ini](configs/app.ini): камеры, получатель UDP, видеовыход, пороги.
Точки калибровки каждой камеры лежат в `configs/calibration/cameraN.csv` (`pixel_x,pixel_y,lng,lat`).
При старте программа печатает ошибку репроекции калибровки по каждой камере.
