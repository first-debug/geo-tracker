#include "glib.h"
#include "gst/gst.h"
#include "gst/gstelement.h"
#include <glib-unix.h>
#include <csignal>
#include <unistd.h>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "bus_call.hpp"
#include "custom_bins.hpp"
#include "app_config.hpp"
#include "probers.hpp"
#include "udp_connection.hpp"

typedef struct {
    GMainLoop *loop;
    GstElement *pipeline;
    gboolean eos_sent;
} SignalData;

// Сколько ждать прохождения EOS по конвейеру и перехода в NULL, секунд
static const guint EOS_TIMEOUT_SEC = 3;
static const guint SHUTDOWN_TIMEOUT_SEC = 10;

static gboolean on_eos_timeout(gpointer user_data) {
    SignalData *data = (SignalData *)user_data;
    g_printerr("EOS did not reach the sinks in %u s, stopping anyway.\n", EOS_TIMEOUT_SEC);
    g_main_loop_quit(data->loop);
    return G_SOURCE_REMOVE;
}

// Первый SIGINT/SIGTERM — отправить EOS, чтобы корректно закрыть энкодеры и
// RTSP-сессии; если EOS не дойдёт за EOS_TIMEOUT_SEC (конвейер завис), выходим
// всё равно. Второй сигнал — выйти сразу.
static gboolean on_terminate_signal(gpointer user_data) {
    SignalData *data = (SignalData *)user_data;
    if (!data->eos_sent) {
        g_print("\nStopping pipeline (send the signal again to force quit)...\n");
        data->eos_sent = TRUE;
        gst_element_send_event(data->pipeline, gst_event_new_eos());
        g_timeout_add_seconds(EOS_TIMEOUT_SEC, on_eos_timeout, data);
    } else {
        g_main_loop_quit(data->loop);
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char *argv[]) {
    GMainLoop *loop = NULL;
    GstElement *pipeline = NULL,
               *streammux = NULL,
               *pgie = NULL,
               *nvosd = NULL,
               *tracker = NULL,
               *demux = NULL;
    GstPad *src_pad = NULL, *sink_pad = NULL;
    GstBus *bus = NULL;
    guint bus_watch_id;

    gst_init(&argc, &argv);

    // Построчная буферизация stdout: иначе при выводе в файл или через tee
    // строки g_print (статистика, сообщения об остановке) теряются при падении.
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc != 2) {
        g_printerr("Usage: %s <path_to_app_config>\n", argv[0]);
        return -1;
    }

    AppConfig config;
    std::string error;
    if (!load_app_config(argv[1], config, error)) {
        g_printerr("Cannot load config: %s\n", error.c_str());
        return -1;
    }

    const int num_sources = static_cast<int>(config.cameras.size());

    // Калибровку загружаем до сборки конвейера, чтобы ошибка в точках
    // обнаруживалась сразу, а не после подключения к камерам.
    UdpConnection udp_conn(config.udp_host, static_cast<uint16_t>(config.udp_port));
    if (!udp_conn.is_open()) {
        g_printerr("Cannot open UDP connection to %s:%d.\n", config.udp_host.c_str(), config.udp_port);
        return -1;
    }

    pgie_probe_data probe_data;
    probe_data.udp_conn = &udp_conn;
    probe_data.min_aspect_ratio = config.min_aspect_ratio;
    probe_data.transformers.reserve(num_sources);

    for (int cam = 0; cam < num_sources; cam++) {
        std::vector<cv::Point2d> pixels, geo;
        if (!load_calibration_points(config.cameras[cam].calibration_file, pixels, geo, error)) {
            g_printerr("Calibration of camera %d failed: %s\n", cam, error.c_str());
            return -1;
        }

        try {
            probe_data.transformers.emplace_back(pixels, geo, config.ransac_threshold_m);
        } catch (const std::exception &e) {
            g_printerr("Calibration of camera %d failed: %s\n", cam, e.what());
            return -1;
        }

        const auto &st = probe_data.transformers.back().reprojection_stats();
        g_print("[CALIB] camera %d: inliers %zu/%zu, reprojection error median %.1f m, p90 %.1f m, max %.1f m\n",
                cam, st.inliers, st.points, st.median_m, st.p90_m, st.max_m);
    }

    loop = g_main_loop_new(NULL, FALSE);

    // Elements initialization
    pipeline = gst_pipeline_new("sheremetevo");

    streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
    pgie = gst_element_factory_make("nvinfer", "primary-infer");

    if (config.output_enabled) {
        demux = gst_element_factory_make("nvstreamdemux", "demux");
    } else {
        // Без видеовыхода разметка и кодирование не нужны: кадры уходят в fakesink.
        demux = gst_element_factory_make("fakesink", "sink");
        if (demux) {
            g_object_set(G_OBJECT(demux), "sync", FALSE, "async", FALSE, NULL);
        }
    }

    if (!pipeline || !streammux || !pgie || !demux) {
        g_printerr("Cannot create some modules.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(pipeline), streammux, pgie, demux, NULL);

    if (config.tracker_enabled) {
        tracker = gst_element_factory_make("nvtracker", "tracker");
        if (!tracker) {
            g_printerr("Cannot create nvtracker.\n");
            return -1;
        }
        g_object_set(G_OBJECT(tracker),
                "ll-lib-file", config.tracker_lib.c_str(),
                "ll-config-file", config.tracker_config.c_str(),
                "tracker-width", config.tracker_width,
                "tracker-height", config.tracker_height,
                NULL);
        gst_bin_add(GST_BIN(pipeline), tracker);
    }

    // Последний элемент анализа: после него метаданные объектов уже окончательные
    GstElement *analytics_tail = tracker ? tracker : pgie;

    // batched-push-timeout: nvstreammux отправляет пачку, не дожидаясь
    // отставшей или упавшей камеры, чтобы остальные камеры продолжали работать.
    g_object_set(G_OBJECT(streammux),
            "batch-size", num_sources,
            "width", config.frame_width,
            "height", config.frame_height,
            "batched-push-timeout", config.batched_push_timeout_us,
            NULL);

    g_object_set(G_OBJECT(pgie), "config-file-path", config.nvinfer_config.c_str(), NULL);

    gchar buffer[40];
    GstElement *new_bin = NULL;
    for (int index = 0; index < num_sources; index++) {
        const CameraConfig &camera = config.cameras[index];
        SourceTransform transform;
        transform.flip_method = camera.flip_method;
        transform.dewarper_config = camera.dewarper_config;

        new_bin = create_source_bin(camera.uri.c_str(), index, transform,
                config.frame_width, config.frame_height);

        // Номер входа связывает камеру с её калибровкой, поэтому пропускать
        // источник нельзя: все следующие камеры получат чужую гомографию.
        if (new_bin == NULL) {
            g_printerr("Cannot create rtsp source bin for uri = %s\n", camera.uri.c_str());
            return -1;
        }

        gst_bin_add_many(GST_BIN(pipeline), new_bin, NULL);

        snprintf(buffer, 17, "sink_%1d", index);
        src_pad = gst_element_get_static_pad(new_bin, "src");
        sink_pad = gst_element_request_pad_simple(streammux, buffer);

        if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
            g_printerr("Cannot link bin_src and streammux.\n");
            gst_object_unref(src_pad);
            gst_object_unref(sink_pad);
            return -1;
        }

        gst_object_unref(src_pad);
        gst_object_unref(sink_pad);
    }

    if (!gst_element_link(streammux, pgie) ||
            (tracker && !gst_element_link(pgie, tracker)) ||
            !gst_element_link(analytics_tail, demux)) {
        g_printerr("Cannot link elements.\n");
        return -1;
    }

    for (int index = 0; config.output_enabled && index < num_sources; index++) {
        std::string output_uri = config.output_uri_prefix + "_" + std::to_string(index);
        new_bin = create_sink_bin(output_uri.c_str(), index);
        snprintf(buffer, 39, "nvosd_%1d", index);
        nvosd = gst_element_factory_make("nvdsosd", buffer);

        // Видеовыход отвязан от анализа очередью, которая выбрасывает старые
        // кадры: если RTSP-сервер не принимает поток или тормозит, детекция и
        // отправка координат продолжают работать.
        snprintf(buffer, 39, "output_queue_%1d", index);
        GstElement *output_queue = gst_element_factory_make("queue", buffer);

        if (new_bin == NULL || nvosd == NULL || output_queue == NULL) {
            g_printerr("Cannot create rtsp sink bin, queue or nvdsosd for uri = %s\n", output_uri.c_str());
            return -1;
        }

        g_object_set(G_OBJECT(output_queue),
                "leaky", 2,  // downstream: выбрасывать самые старые кадры
                "max-size-buffers", 4,
                "max-size-bytes", 0,
                "max-size-time", (guint64)0,
                NULL);

        gst_bin_add_many(GST_BIN(pipeline), output_queue, nvosd, new_bin, NULL);

        if (!gst_element_link(output_queue, nvosd)) {
            g_printerr("Cannot link output queue and nvosd.\n");
            return -1;
        }

        snprintf(buffer, 17, "src_%1d", index);
        src_pad = gst_element_request_pad_simple(demux, buffer);
        sink_pad = gst_element_get_static_pad(output_queue, "sink");

        if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
            g_printerr("Cannot link demux and output queue.\n");
            gst_object_unref(src_pad);
            gst_object_unref(sink_pad);
            gst_bin_remove_many(GST_BIN(pipeline), nvosd, new_bin, NULL);
            return -1;
        }

        if (!gst_element_link(nvosd, new_bin)) {
            g_printerr("Cannot link nvosd and new sink bin.\n");
            gst_object_unref(src_pad);
            gst_object_unref(sink_pad);
            gst_bin_remove_many(GST_BIN(pipeline), nvosd, new_bin, NULL);
            return -1;
        }

        gst_object_unref(src_pad);
        gst_object_unref(sink_pad);
    }

    // Проба стоит после трекера, чтобы в сообщения попадали его object_id
    set_probe(analytics_tail, "src", pgie_src_pad_buffer_probe, &probe_data);

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "sheremetevo");
    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    BusCallData data = { loop, pipeline };
    bus_watch_id = gst_bus_add_watch(bus, bus_call, &data);
    gst_object_unref(bus);

    SignalData signal_data = { loop, pipeline, FALSE };
    guint sigint_id = g_unix_signal_add(SIGINT, on_terminate_signal, &signal_data);
    guint sigterm_id = g_unix_signal_add(SIGTERM, on_terminate_signal, &signal_data);

    probe_data.init_stats(num_sources);
    g_timeout_add_seconds(5, print_stream_stats, &probe_data);

    g_print("\nStarting pipline...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(loop);

    // Обработчики сигналов GLib работают только внутри главного цикла, а он уже
    // остановлен. Возвращаем стандартное поведение, чтобы Ctrl+C убивал процесс,
    // если какой-то элемент завис при остановке, и страхуемся таймером.
    g_source_remove(sigint_id);
    g_source_remove(sigterm_id);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    alarm(SHUTDOWN_TIMEOUT_SEC);

    g_print("Stopping pipeline...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    alarm(0);

    gst_object_unref(GST_OBJECT(pipeline));
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);

    return 0;
}
