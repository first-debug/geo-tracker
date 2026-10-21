#ifndef PROBERS_H
#define PROBERS_H

#include "gst/gstpad.h"
#include <atomic>
#include <memory>
#include <vector>

#include "geo_zone.hpp"
#include "pixel_transformer.hpp"
#include "points_sender.hpp"

struct pgie_probe_data {
    PointsSender *sender = nullptr;
    // гомография для каждой камеры, индекс — номер входа nvstreammux (pad_index)
    std::vector<PixelGeoTransformer> transformers;
    // зона, вне которой точки не отправляются; пустая — без фильтра
    GeoZone zone;
    // минимальное отношение ширины bbox к высоте
    double min_aspect_ratio = 1.2;

    // Счётчики для периодической статистики, по одному на камеру.
    // Пишутся из потока конвейера, читаются и обнуляются таймером в главном цикле.
    size_t num_streams = 0;
    std::unique_ptr<std::atomic<guint64>[]> frames;
    std::unique_ptr<std::atomic<guint64>[]> objects;

    void init_stats(size_t streams) {
        num_streams = streams;
        frames.reset(new std::atomic<guint64>[streams]());
        objects.reset(new std::atomic<guint64>[streams]());
    }
};

// Печатает fps и число объектов по каждой камере за прошедший интервал.
// Предназначена для g_timeout_add_seconds, user_data — pgie_probe_data.
gboolean print_stream_stats(gpointer user_data);

void set_probe(GstElement* element, const gchar *pad_name, GstPadProbeReturn (*prober) (GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data),
        gpointer u_data);

GstPadProbeReturn print_nvmeta_probe(GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data);

GstPadProbeReturn pgie_src_pad_buffer_probe(GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data);

#endif // PROBERS_H
