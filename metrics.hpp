#ifndef METRICS_H
#define METRICS_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <glib.h>

#include "points_sender.hpp"
#include "probers.hpp"
#include "source_manager.hpp"

/*
 * Метрики в формате Prometheus: GET http://<bind-address>:<port>/metrics.
 *
 * Текст страницы собирается в главном цикле раз в секунду (там же живёт
 * состояние камер), а отдельный поток только раздаёт готовый текст, поэтому
 * медленный клиент не задерживает ни конвейер, ни главный цикл.
 * Список метрик — docs/monitoring.md.
 */
struct MetricsServer {
    std::string bind_address = "0.0.0.0";
    int port = 9464;

    // источники данных, все необязательны
    pgie_probe_data *probe = NULL;
    SourceManager *sources = NULL;
    PointsSender *sender = NULL;

    // внутреннее состояние
    gint64 started_us = 0;
    int listen_fd = -1;
    guint timer_id = 0;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::string page;
};

// Открывает порт и запускает раздачу. Вызывать после настройки источников.
bool metrics_start(MetricsServer &metrics, std::string &error);

// Останавливает поток и закрывает порт.
void metrics_stop(MetricsServer &metrics);

// Текст страницы /metrics по текущему состоянию; только для главного цикла.
std::string metrics_render(const MetricsServer &metrics);

#endif // METRICS_H
