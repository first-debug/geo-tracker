#ifndef LOG_H
#define LOG_H

#include <string>

#include <glib.h>

/*
 * Журнал приложения.
 *
 * Каждая запись — одна строка с временем, уровнем, компонентом и, если
 * запись про камеру, её номером:
 *   text: 2026-10-21T17:31:45.123+03:00 WARN  [RTSP] camera=2 no frames for 10 s, ...
 *   json: {"ts":"2026-10-21T17:31:45.123+03:00","level":"warn","component":"RTSP","camera":2,"msg":"..."}
 * debug и info идут в stdout, warn и error — в stderr (Docker собирает оба),
 * и, если задан файл, в файл с ротацией по размеру.
 *
 * log_init() перехватывает g_print/g_printerr: старые сообщения вида
 * "[TAG] текст" тоже попадают в журнал (g_print — info, g_printerr — error,
 * тег становится компонентом).
 */

enum class LogLevel { Debug = 0, Info, Warning, Error };

struct LogConfig {
    LogLevel level = LogLevel::Info;
    bool json = false;
    // пусто — только stdout/stderr
    std::string file;
    // ротация файла: при превышении размера file -> file.1 -> ... -> file.<max_files>
    guint max_size_mb = 50;
    guint max_files = 5;
};

// Разбор имени уровня (debug, info, warning/warn, error). false — неизвестное имя.
bool log_level_from_string(const std::string &name, LogLevel &level);

// Включает журнал с настройками по умолчанию; вызывать первым делом в main.
void log_init();

// Применяет настройки из конфига. При ошибке открытия файла журнал остаётся
// на stdout/stderr и возвращается false с причиной в error.
bool log_configure(const LogConfig &config, std::string &error);

// Закрывает файл журнала.
void log_shutdown();

// Потокобезопасны. camera < 0 — запись не про конкретную камеру.
void log_write(LogLevel level, const char *component, int camera, const char *format, ...)
        G_GNUC_PRINTF(4, 5);

#define LOG_DEBUG(component, ...) log_write(LogLevel::Debug, component, -1, __VA_ARGS__)
#define LOG_INFO(component, ...) log_write(LogLevel::Info, component, -1, __VA_ARGS__)
#define LOG_WARN(component, ...) log_write(LogLevel::Warning, component, -1, __VA_ARGS__)
#define LOG_ERROR(component, ...) log_write(LogLevel::Error, component, -1, __VA_ARGS__)

#define LOG_CAM_INFO(component, camera, ...) log_write(LogLevel::Info, component, camera, __VA_ARGS__)
#define LOG_CAM_WARN(component, camera, ...) log_write(LogLevel::Warning, component, camera, __VA_ARGS__)
#define LOG_CAM_ERROR(component, camera, ...) log_write(LogLevel::Error, component, camera, __VA_ARGS__)

#endif // LOG_H
