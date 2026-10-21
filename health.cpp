#include <glib/gstdio.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "health.hpp"

static const guint HEARTBEAT_INTERVAL_SEC = 2;
static const gint64 US_PER_SEC = 1000000;

static GstPadProbeReturn frame_probe(GstPad *, GstPadProbeInfo *, gpointer user_data) {
    HealthMonitor *health = static_cast<HealthMonitor *>(user_data);
    health->last_frame_us = g_get_monotonic_time();
    return GST_PAD_PROBE_OK;
}

void health_attach(HealthMonitor &health, GstElement *analytics_tail) {
    GstPad *pad = gst_element_get_static_pad(analytics_tail, "src");
    if (pad == NULL) {
        g_printerr("[HEALTH] Cannot get src pad of %s\n", GST_ELEMENT_NAME(analytics_tail));
        return;
    }
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, frame_probe, &health, NULL);
    gst_object_unref(pad);
}

static void write_heartbeat(HealthMonitor &health, gint64 now) {
    gint64 last_frame = health.last_frame_us;
    double frame_age = last_frame ? (now - last_frame) / 1e6 : -1.0;

    size_t receiving = 0, total = 0;
    if (health.sources != NULL) {
        for (const auto &slot : health.sources->slots) {
            total++;
            gint64 last = slot->last_buffer_us;
            if (slot->state == SourceSlot::State::Running && last != 0 &&
                    now - last < (gint64)health.stall_timeout_sec * US_PER_SEC) {
                receiving++;
            }
        }
    }

    // last_frame_age_sec = -1, пока не было ни одного кадра
    gchar *contents = g_strdup_printf(
            "time=%" G_GINT64_FORMAT "\n"
            "uptime_sec=%" G_GINT64_FORMAT "\n"
            "last_frame_age_sec=%.1f\n"
            "cameras_receiving=%zu/%zu\n",
            g_get_real_time() / US_PER_SEC,
            (now - health.started_us) / US_PER_SEC,
            frame_age, receiving, total);

    // g_file_set_contents пишет во временный файл и переименовывает его,
    // поэтому healthcheck никогда не прочитает файл наполовину
    GError *err = NULL;
    if (!g_file_set_contents(health.heartbeat_file.c_str(), contents, -1, &err)) {
        g_printerr("[HEALTH] Cannot write %s: %s\n", health.heartbeat_file.c_str(), err->message);
        g_error_free(err);
    }
    g_free(contents);
}

static gboolean health_tick(gpointer user_data) {
    HealthMonitor &health = *static_cast<HealthMonitor *>(user_data);
    gint64 now = g_get_monotonic_time();
    health.last_tick_us = now;

    if (!health.heartbeat_file.empty()) {
        write_heartbeat(health, now);
    }

    if (health.no_frames_restart_sec > 0 && !health.failed) {
        gint64 last_frame = health.last_frame_us;
        gint64 since = last_frame ? last_frame : health.started_us;
        if (now - since > (gint64)health.no_frames_restart_sec * US_PER_SEC) {
            g_printerr("[HEALTH] No frames processed for %u s, restarting the application.\n",
                    health.no_frames_restart_sec);
            health.failed = true;
            if (health.on_fatal != NULL) {
                health.on_fatal(health.on_fatal_data);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

// Отдельный поток: главный цикл мог зависнуть, и тогда штатно выйти уже
// нельзя, остаётся только завершить процесс.
static void watchdog_loop(HealthMonitor *health) {
    std::unique_lock<std::mutex> lock(health->mutex);
    while (!health->cv.wait_for(lock, std::chrono::seconds(1), [health] { return health->stopping; })) {
        gint64 silence = g_get_monotonic_time() - health->last_tick_us;
        if (silence > (gint64)health->hang_timeout_sec * US_PER_SEC) {
            fprintf(stderr, "[HEALTH] Main loop is not responding for %u s, exiting.\n",
                    health->hang_timeout_sec);
            fflush(stderr);
            _exit(EXIT_FAILURE);
        }
    }
}

void health_start(HealthMonitor &health) {
    health.started_us = g_get_monotonic_time();
    health.last_tick_us = health.started_us;

    // Файл прошлого запуска (контейнер перезапускается с той же /tmp)
    // не должен выдавать новый процесс за здоровый.
    if (!health.heartbeat_file.empty()) {
        g_unlink(health.heartbeat_file.c_str());
    }

    health.timer_id = g_timeout_add_seconds(HEARTBEAT_INTERVAL_SEC, health_tick, &health);

    if (health.hang_timeout_sec > 0) {
        health.stopping = false;
        health.watchdog = std::thread(watchdog_loop, &health);
    }
}

void health_stop(HealthMonitor &health) {
    if (health.watchdog.joinable()) {
        {
            std::lock_guard<std::mutex> lock(health.mutex);
            health.stopping = true;
        }
        health.cv.notify_all();
        health.watchdog.join();
    }

    if (health.timer_id != 0) {
        g_source_remove(health.timer_id);
        health.timer_id = 0;
    }

    if (!health.heartbeat_file.empty()) {
        g_unlink(health.heartbeat_file.c_str());
    }
}
