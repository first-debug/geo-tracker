#ifndef HEALTH_H
#define HEALTH_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <gst/gst.h>

#include "source_manager.hpp"

/*
 * Контроль работоспособности для Docker HEALTHCHECK и автоперезапуска.
 *
 *  - Раз в HEARTBEAT_INTERVAL_SEC главный цикл записывает heartbeat-файл:
 *    время, возраст последнего обработанного кадра, сколько камер шлют кадры.
 *    docker/healthcheck.sh по нему решает, здоров ли контейнер.
 *  - Отдельный поток следит, что главный цикл не завис: если файл не
 *    обновлялся дольше hang_timeout_sec, процесс завершается с ошибкой,
 *    и Docker перезапускает контейнер (restart policy).
 *  - Если ни одна камера не дала кадр на выходе анализа дольше
 *    no_frames_restart_sec, вызывается on_fatal (штатная остановка),
 *    и приложение завершается с ошибкой для перезапуска.
 * Нулевые таймауты и пустой heartbeat_file отключают соответствующую часть.
 */
struct HealthMonitor {
    std::string heartbeat_file;
    guint hang_timeout_sec = 60;
    guint no_frames_restart_sec = 300;

    SourceManager *sources = NULL;

    // штатная остановка приложения при отсутствии кадров
    void (*on_fatal)(gpointer) = NULL;
    gpointer on_fatal_data = NULL;
    // приложение остановлено монитором, код выхода должен быть ненулевым
    bool failed = false;

    // по g_get_monotonic_time; last_frame_us пишется из потока конвейера
    std::atomic<gint64> last_frame_us{0};
    std::atomic<gint64> last_tick_us{0};
    gint64 started_us = 0;

    guint timer_id = 0;
    std::thread watchdog;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
};

// Проба на выходе анализа (после nvinfer/nvtracker): отмечает время кадров.
void health_attach(HealthMonitor &health, GstElement *analytics_tail);

// Запускает таймер heartbeat и сторожевой поток. Вызывать перед g_main_loop_run.
void health_start(HealthMonitor &health);

// Останавливает сторожевой поток и удаляет heartbeat-файл.
// Вызывать сразу после выхода из g_main_loop_run.
void health_stop(HealthMonitor &health);

#endif // HEALTH_H
