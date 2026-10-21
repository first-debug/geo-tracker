#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <glib/gstdio.h>

#include "log.hpp"

namespace {

struct LogState {
    std::mutex mutex;
    LogConfig config;
    FILE *file = NULL;
    // текущий размер файла журнала, байт
    size_t file_size = 0;
};

LogState &state() {
    static LogState instance;
    return instance;
}

const char *level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

// Время с миллисекундами и часовым поясом: 2026-10-21T17:31:45.123+03:00
std::string timestamp() {
    GDateTime *now = g_date_time_new_now_local();
    gchar *date = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S");
    gchar *zone = g_date_time_format(now, "%:z");
    gchar *result = g_strdup_printf("%s.%03d%s", date, g_date_time_get_microsecond(now) / 1000, zone);
    std::string out(result);
    g_free(result);
    g_free(zone);
    g_free(date);
    g_date_time_unref(now);
    return out;
}

void append_json_string(std::string &out, const std::string &value) {
    out += '"';
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

// Одна запись — одна строка: внутренние переводы строк (например, отладочная
// информация в ошибках GStreamer) заменяются на " | ", крайние пробелы убираются.
std::string single_line(const char *text) {
    std::string out;
    const char *begin = text;
    const char *end = text + strlen(text);
    while (begin < end && g_ascii_isspace(*begin)) {
        begin++;
    }
    while (end > begin && g_ascii_isspace(end[-1])) {
        end--;
    }
    for (const char *p = begin; p < end; p++) {
        if (*p == '\n') {
            while (!out.empty() && out.back() == ' ') {
                out.pop_back();
            }
            out += " | ";
            while (p + 1 < end && g_ascii_isspace(p[1])) {
                p++;
            }
        } else if (*p != '\r') {
            out += *p;
        }
    }
    return out;
}

std::string format_record(const LogConfig &config, LogLevel level, const char *component,
        int camera, const std::string &message) {
    std::string line;
    line.reserve(message.size() + 96);

    if (config.json) {
        line += "{\"ts\":\"";
        line += timestamp();
        line += "\",\"level\":\"";
        line += level_name(level);
        line += "\"";
        if (component != NULL && *component != '\0') {
            line += ",\"component\":";
            append_json_string(line, component);
        }
        if (camera >= 0) {
            line += ",\"camera\":" + std::to_string(camera);
        }
        line += ",\"msg\":";
        append_json_string(line, message);
        line += "}\n";
        return line;
    }

    char level_buf[8];
    snprintf(level_buf, sizeof(level_buf), "%-5s", level_name(level));
    for (char *p = level_buf; *p; p++) {
        *p = g_ascii_toupper(*p);
    }

    line += timestamp();
    line += ' ';
    line += level_buf;
    if (component != NULL && *component != '\0') {
        line += " [";
        line += component;
        line += ']';
    }
    if (camera >= 0) {
        line += " camera=" + std::to_string(camera);
    }
    line += ' ';
    line += message;
    line += '\n';
    return line;
}

bool open_file(LogState &st, std::string &error) {
    st.file = g_fopen(st.config.file.c_str(), "a");
    if (st.file == NULL) {
        error = st.config.file + ": " + g_strerror(errno);
        return false;
    }
    setvbuf(st.file, NULL, _IOLBF, 0);
    fseek(st.file, 0, SEEK_END);
    long size = ftell(st.file);
    st.file_size = size > 0 ? static_cast<size_t>(size) : 0;
    return true;
}

// file.<max_files> удаляется, file.N -> file.N+1, file -> file.1
void rotate(LogState &st) {
    fclose(st.file);
    st.file = NULL;

    const std::string &path = st.config.file;
    if (st.config.max_files == 0) {
        g_unlink(path.c_str());
    } else {
        g_unlink((path + "." + std::to_string(st.config.max_files)).c_str());
        for (guint i = st.config.max_files - 1; i >= 1; i--) {
            g_rename((path + "." + std::to_string(i)).c_str(),
                     (path + "." + std::to_string(i + 1)).c_str());
        }
        g_rename(path.c_str(), (path + ".1").c_str());
    }

    std::string error;
    if (!open_file(st, error)) {
        fprintf(stderr, "log: cannot reopen %s after rotation, logging to stdout only\n", error.c_str());
    }
}

void write_record(LogLevel level, const char *component, int camera, const std::string &message) {
    LogState &st = state();
    std::lock_guard<std::mutex> lock(st.mutex);

    if (level < st.config.level || message.empty()) {
        return;
    }

    std::string line = format_record(st.config, level, component, camera, message);

    FILE *console = level >= LogLevel::Warning ? stderr : stdout;
    fputs(line.c_str(), console);
    fflush(console);

    if (st.file != NULL) {
        size_t max_size = static_cast<size_t>(st.config.max_size_mb) * 1024 * 1024;
        if (max_size > 0 && st.file_size > 0 && st.file_size + line.size() > max_size) {
            rotate(st);
        }
        if (st.file != NULL) {
            fputs(line.c_str(), st.file);
            st.file_size += line.size();
        }
    }
}

// Старые сообщения "[TAG] текст": тег из заглавных букв, цифр и '_'
// становится компонентом записи.
void write_legacy(LogLevel level, const gchar *text) {
    std::string message = single_line(text);
    std::string component;

    if (message.size() > 2 && message[0] == '[') {
        size_t close = message.find(']');
        bool is_tag = close != std::string::npos && close > 1 && close <= 16;
        for (size_t i = 1; is_tag && i < close; i++) {
            char c = message[i];
            is_tag = g_ascii_isupper(c) || g_ascii_isdigit(c) || c == '_';
        }
        if (is_tag) {
            component = message.substr(1, close - 1);
            size_t rest = message.find_first_not_of(' ', close + 1);
            message = rest == std::string::npos ? std::string() : message.substr(rest);
        }
    }

    write_record(level, component.c_str(), -1, message);
}

void print_handler(const gchar *text) {
    write_legacy(LogLevel::Info, text);
}

void printerr_handler(const gchar *text) {
    write_legacy(LogLevel::Error, text);
}

} // namespace

bool log_level_from_string(const std::string &name, LogLevel &level) {
    if (name == "debug") {
        level = LogLevel::Debug;
    } else if (name == "info") {
        level = LogLevel::Info;
    } else if (name == "warning" || name == "warn") {
        level = LogLevel::Warning;
    } else if (name == "error") {
        level = LogLevel::Error;
    } else {
        return false;
    }
    return true;
}

void log_init() {
    g_set_print_handler(print_handler);
    g_set_printerr_handler(printerr_handler);
}

bool log_configure(const LogConfig &config, std::string &error) {
    LogState &st = state();
    std::lock_guard<std::mutex> lock(st.mutex);

    if (st.file != NULL) {
        fclose(st.file);
        st.file = NULL;
    }
    st.config = config;

    if (config.file.empty()) {
        return true;
    }
    if (!open_file(st, error)) {
        st.config.file.clear();
        return false;
    }
    return true;
}

void log_shutdown() {
    LogState &st = state();
    std::lock_guard<std::mutex> lock(st.mutex);
    if (st.file != NULL) {
        fclose(st.file);
        st.file = NULL;
    }
}

void log_write(LogLevel level, const char *component, int camera, const char *format, ...) {
    va_list args;
    va_start(args, format);
    gchar *text = g_strdup_vprintf(format, args);
    va_end(args);

    write_record(level, component, camera, single_line(text));
    g_free(text);
}
