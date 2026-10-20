#ifndef CUSTOM_BINS_H
#define CUSTOM_BINS_H

#include <string>

#include "gst/gstelement.h"

// Преобразования входного потока. Незаданные преобразования в конвейер не добавляются.
struct SourceTransform {
    // flip-method для nvvideoconvert (0..7), -1 — без поворота
    int flip_method = -1;
    // конфиг nvdewarper, пустая строка — без выпрямления
    std::string dewarper_config;
};

GstElement *create_source_bin(const gchar *uri, gint index, const SourceTransform &transform,
        gint width, gint height);
GstElement *create_sink_bin(const gchar *uri, gint index);

#endif // CUSTOM_BINS_H
