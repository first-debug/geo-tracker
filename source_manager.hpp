#ifndef SOURCE_MANAGER_H
#define SOURCE_MANAGER_H

#include <atomic>
#include <memory>
#include <vector>

#include <gst/gst.h>

/*
 * Переподключение камер.
 *
 * Камера перезапускается (её source-bin переводится в NULL, а через
 * reconnect_interval_sec обратно в состояние конвейера), если:
 *  - элемент внутри её source-bin прислал ERROR (обрыв связи, сервер недоступен);
 *  - от неё нет кадров дольше stall_timeout_sec (сервер молча перестал слать данные).
 * Остальные камеры в это время работают: nvstreammux собирает пачки без неё.
 * Номер камеры (вход nvstreammux) при перезапуске не меняется, поэтому
 * camera_id и калибровка остаются прежними.
 */

struct SourceManager;

struct SourceSlot {
    enum class State { Running, Stopped };

    SourceManager *manager = NULL;
    GstElement *bin = NULL;
    guint index = 0;

    State state = State::Running;
    // когда камера перешла в текущее состояние, по g_get_monotonic_time
    gint64 state_since_us = 0;
    // время последнего кадра; пишется из потока конвейера
    std::atomic<gint64> last_buffer_us{0};
    // сколько раз подряд камера отваливалась, обнуляется при первом кадре
    guint failures = 0;
};

struct SourceManager {
    guint stall_timeout_sec = 10;
    guint reconnect_interval_sec = 5;
    // при остановке приложения EOS от камер должен дойти до выхода
    std::atomic<bool> shutting_down{false};
    std::vector<std::unique_ptr<SourceSlot>> slots;
};

// Добавляет source-bin в конвейер и подключает его ко входу sink_<index> nvstreammux.
// Индексы нужно добавлять подряд с нуля.
bool source_manager_add(SourceManager &manager, GstElement *pipeline, GstElement *streammux,
        GstElement *bin, guint index);

// Вызывается из обработчика шины, если ERROR пришёл изнутри source-bin.
void source_manager_on_error(SourceManager &manager, GstElement *bin, const gchar *reason);

// Перед отправкой EOS при остановке: EOS от камер больше не отбрасывается,
// а за остановленные камеры EOS отправляется прямо в nvstreammux.
void source_manager_shutdown(SourceManager &manager);

// Таймер главного цикла (раз в секунду), user_data — SourceManager:
// останавливает зависшие камеры и запускает остановленные.
gboolean source_manager_watchdog(gpointer user_data);

#endif // SOURCE_MANAGER_H
