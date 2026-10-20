#include "glib.h"
#include "gst/gst.h"
#include "gst/gstelement.h"
#include <glib-unix.h>
#include <csignal>
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

// Первый SIGINT/SIGTERM — отправить EOS, чтобы корректно закрыть энкодеры и
// RTSP-сессии; второй — выйти сразу.
static gboolean on_terminate_signal(gpointer user_data) {
    SignalData *data = (SignalData *)user_data;
    if (!data->eos_sent) {
        g_print("\nStopping pipeline (send the signal again to force quit)...\n");
        data->eos_sent = TRUE;
        gst_element_send_event(data->pipeline, gst_event_new_eos());
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
               *demux = NULL;
    GstPad *src_pad = NULL, *sink_pad = NULL;
    GstBus *bus = NULL;
    guint bus_watch_id;

    gst_init(&argc, &argv);

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

    if (!gst_element_link_many(streammux, pgie, demux, NULL)) {
        g_printerr("Cannot link elements.\n");
        return -1;
    }

    for (int index = 0; config.output_enabled && index < num_sources; index++) {
        std::string output_uri = config.output_uri_prefix + "_" + std::to_string(index);
        new_bin = create_sink_bin(output_uri.c_str(), index);
        snprintf(buffer, 39, "nvosd_%1d", index);
        nvosd = gst_element_factory_make("nvdsosd", buffer);

        if (new_bin == NULL || nvosd == NULL) {
            g_printerr("Cannot create rtsp sink bin or nvdsosd for uri = %s\n", output_uri.c_str());
            return -1;
        }

        gst_bin_add_many(GST_BIN(pipeline), nvosd, new_bin, NULL);

        snprintf(buffer, 17, "src_%1d", index);
        src_pad = gst_element_request_pad_simple(demux, buffer);
        sink_pad = gst_element_get_static_pad(nvosd, "sink");

        if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
            g_printerr("Cannot link demux and nvosd.\n");
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

    set_probe(pgie, "src", pgie_src_pad_buffer_probe, &probe_data);

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "sheremetevo");
    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    BusCallData data = { loop, pipeline };
    bus_watch_id = gst_bus_add_watch(bus, bus_call, &data);
    gst_object_unref(bus);

    SignalData signal_data = { loop, pipeline, FALSE };
    guint sigint_id = g_unix_signal_add(SIGINT, on_terminate_signal, &signal_data);
    guint sigterm_id = g_unix_signal_add(SIGTERM, on_terminate_signal, &signal_data);

    g_print("\nStarting pipline...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(loop);

    g_print("Stopping pipeline...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);

    g_source_remove(sigint_id);
    g_source_remove(sigterm_id);

    gst_object_unref(GST_OBJECT(pipeline));
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);

    return 0;
}
