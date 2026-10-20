#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <string>
#include <vector>

#include "opencv2/core/types.hpp"

struct CameraConfig {
    std::string uri;
    // Преобразования входного потока; незаданные не добавляются в конвейер.
    // flip-method для nvvideoconvert (0..7), -1 — без поворота
    int flip_method = -1;
    // конфиг nvdewarper, пустая строка — без выпрямления
    std::string dewarper_config;
    // CSV с калибровочными точками: pixel_x,pixel_y,lng,lat
    std::string calibration_file;
};

struct AppConfig {
    std::string nvinfer_config;

    // размер кадров в пачке nvstreammux (и выхода nvdewarper, если он включён)
    int frame_width = 1728;
    int frame_height = 2752;
    int batched_push_timeout_us = 40000;

    // видеовыход с разметкой: <output_uri_prefix>_<номер камеры>
    bool output_enabled = true;
    std::string output_uri_prefix;

    std::string udp_host;
    int udp_port = 0;

    double ransac_threshold_m = 10.0;
    // минимальное отношение ширины bbox к высоте, чтобы объект считался самолётом
    double min_aspect_ratio = 1.2;

    // порядок камер задаёт camera_id: [camera0], [camera1], ...
    std::vector<CameraConfig> cameras;

    // Трекер (nvtracker). Если секции [tracker] нет или enabled=false,
    // трекер в конвейер не добавляется и object_id в сообщениях равен -1.
    bool tracker_enabled = false;
    std::string tracker_lib;     // ll-lib-file
    std::string tracker_config;  // ll-config-file
    int tracker_width = 960;     // кратно 32
    int tracker_height = 544;    // кратно 32
};

/*
 * Загружает конфиг приложения (формат INI, как у nvinfer).
 * Относительные пути в конфиге считаются от папки, где лежит сам конфиг.
 * При ошибке возвращает false и пишет причину в error.
 */
bool load_app_config(const std::string &path, AppConfig &config, std::string &error);

/*
 * Читает CSV с калибровочными точками. Строки: pixel_x,pixel_y,lng,lat.
 * Пустые строки, строки с '#' и строка заголовка пропускаются.
 */
bool load_calibration_points(const std::string &path,
                             std::vector<cv::Point2d> &pixels,
                             std::vector<cv::Point2d> &geo,
                             std::string &error);

#endif // APP_CONFIG_H
