#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "log.hpp"
#include "metrics.hpp"

namespace {

const guint RENDER_INTERVAL_SEC = 1;
// сколько ждать запрос от клиента, прежде чем закрыть соединение
const int CLIENT_TIMEOUT_SEC = 2;
const size_t MAX_REQUEST_BYTES = 8192;

void family(std::string &out, const char *name, const char *type, const char *help) {
    out += "# HELP ";
    out += name;
    out += ' ';
    out += help;
    out += "\n# TYPE ";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
}

void sample(std::string &out, const char *name, int camera, const char *format, ...) G_GNUC_PRINTF(4, 5);

// Строка "<name>{camera="N"} <value>"; camera < 0 — без метки
void sample(std::string &out, const char *name, int camera, const char *format, ...) {
    char value[64];
    va_list args;
    va_start(args, format);
    vsnprintf(value, sizeof(value), format, args);
    va_end(args);

    out += name;
    if (camera >= 0) {
        out += "{camera=\"" + std::to_string(camera) + "\"}";
    }
    out += ' ';
    out += value;
    out += '\n';
}

gboolean render_tick(gpointer user_data) {
    MetricsServer &metrics = *static_cast<MetricsServer *>(user_data);
    std::string page = metrics_render(metrics);
    std::lock_guard<std::mutex> lock(metrics.mutex);
    metrics.page.swap(page);
    return G_SOURCE_CONTINUE;
}

void send_all(int fd, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

void respond(int fd, const char *status, const char *content_type, const std::string &body, bool head) {
    std::string response = std::string("HTTP/1.1 ") + status + "\r\n"
            "Content-Type: " + content_type + "\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n";
    if (!head) {
        response += body;
    }
    send_all(fd, response);
}

void handle_client(MetricsServer &metrics, int fd) {
    struct timeval timeout = { CLIENT_TIMEOUT_SEC, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // Нужна только строка запроса: "GET /metrics HTTP/1.1"
    std::string request;
    char buf[1024];
    while (request.find("\r\n") == std::string::npos && request.size() < MAX_REQUEST_BYTES) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return;
        }
        request.append(buf, static_cast<size_t>(n));
    }

    std::string line = request.substr(0, request.find("\r\n"));
    size_t sp1 = line.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        respond(fd, "400 Bad Request", "text/plain", "bad request\n", false);
        return;
    }

    std::string method = line.substr(0, sp1);
    std::string path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    path = path.substr(0, path.find('?'));
    bool head = method == "HEAD";

    if (method != "GET" && !head) {
        respond(fd, "405 Method Not Allowed", "text/plain", "only GET is supported\n", false);
    } else if (path == "/metrics") {
        std::string page;
        {
            std::lock_guard<std::mutex> lock(metrics.mutex);
            page = metrics.page;
        }
        respond(fd, "200 OK", "text/plain; version=0.0.4; charset=utf-8", page, head);
    } else if (path == "/") {
        respond(fd, "200 OK", "text/plain; charset=utf-8", "geotracker: metrics at /metrics\n", head);
    } else {
        respond(fd, "404 Not Found", "text/plain", "not found\n", head);
    }
}

void serve(MetricsServer *metrics) {
    while (!metrics->stopping) {
        struct pollfd pfd = { metrics->listen_fd, POLLIN, 0 };
        int ready = poll(&pfd, 1, 500);
        if (ready <= 0) {
            continue;
        }
        int client = accept(metrics->listen_fd, NULL, NULL);
        if (client < 0) {
            continue;
        }
        handle_client(*metrics, client);
        close(client);
    }
}

} // namespace

std::string metrics_render(const MetricsServer &metrics) {
    std::string out;
    out.reserve(4096);
    const gint64 now = g_get_monotonic_time();

    family(out, "geo_tracker_uptime_seconds", "gauge", "Seconds since the application started.");
    sample(out, "geo_tracker_uptime_seconds", -1, "%.0f", (now - metrics.started_us) / 1e6);

    if (metrics.sources != NULL) {
        const SourceManager &sources = *metrics.sources;

        family(out, "geo_tracker_camera_up", "gauge",
                "1 if the camera delivers frames (a frame within [rtsp] stall-timeout-sec), else 0.");
        for (const auto &slot : sources.slots) {
            sample(out, "geo_tracker_camera_up", static_cast<int>(slot->index), "%d",
                    source_manager_is_receiving(sources, *slot, now) ? 1 : 0);
        }

        family(out, "geo_tracker_camera_last_frame_age_seconds", "gauge",
                "Seconds since the last frame from the camera, -1 if there were no frames yet.");
        for (const auto &slot : sources.slots) {
            gint64 last = slot->last_buffer_us;
            sample(out, "geo_tracker_camera_last_frame_age_seconds", static_cast<int>(slot->index), "%.1f",
                    last ? (now - last) / 1e6 : -1.0);
        }

        family(out, "geo_tracker_camera_restarts_total", "counter",
                "Camera restarts after an error or a stall.");
        for (const auto &slot : sources.slots) {
            sample(out, "geo_tracker_camera_restarts_total", static_cast<int>(slot->index),
                    "%" G_GUINT64_FORMAT, slot->restarts_total);
        }

        family(out, "geo_tracker_camera_down_seconds", "gauge",
                "Seconds the camera has been down in the current outage, 0 if it works.");
        for (const auto &slot : sources.slots) {
            double down = slot->failures > 0 ? (now - slot->lost_since_us) / 1e6 : 0.0;
            sample(out, "geo_tracker_camera_down_seconds", static_cast<int>(slot->index), "%.0f", down);
        }
    }

    if (metrics.probe != NULL && metrics.probe->counters) {
        const pgie_probe_data &probe = *metrics.probe;

        family(out, "geo_tracker_camera_fps", "gauge",
                "Frames per second at the analytics output over the last statistics interval (5 s).");
        for (size_t i = 0; i < probe.num_streams; i++) {
            sample(out, "geo_tracker_camera_fps", static_cast<int>(i), "%.2f", probe.fps[i]);
        }

        family(out, "geo_tracker_camera_frames_total", "counter", "Frames processed by the analytics.");
        for (size_t i = 0; i < probe.num_streams; i++) {
            sample(out, "geo_tracker_camera_frames_total", static_cast<int>(i),
                    "%" G_GUINT64_FORMAT, (guint64)probe.counters[i].frames);
        }

        family(out, "geo_tracker_camera_objects_total", "counter", "Objects detected on processed frames.");
        for (size_t i = 0; i < probe.num_streams; i++) {
            sample(out, "geo_tracker_camera_objects_total", static_cast<int>(i),
                    "%" G_GUINT64_FORMAT, (guint64)probe.counters[i].objects);
        }

        family(out, "geo_tracker_camera_latency_seconds", "summary",
                "Time from frame arrival at nvstreammux to the end of analytics.");
        for (size_t i = 0; i < probe.num_streams; i++) {
            sample(out, "geo_tracker_camera_latency_seconds_sum", static_cast<int>(i), "%.6f",
                    probe.counters[i].latency_sum_us / 1e6);
            sample(out, "geo_tracker_camera_latency_seconds_count", static_cast<int>(i),
                    "%" G_GUINT64_FORMAT, (guint64)probe.counters[i].latency_count);
        }
    }

    if (metrics.sender != NULL) {
        family(out, "geo_tracker_udp_datagrams_sent_total", "counter", "UDP datagrams sent to the receiver.");
        sample(out, "geo_tracker_udp_datagrams_sent_total", -1, "%" G_GUINT64_FORMAT,
                (guint64)metrics.sender->datagrams_sent);
        family(out, "geo_tracker_udp_send_errors_total", "counter", "UDP datagrams that failed to send.");
        sample(out, "geo_tracker_udp_send_errors_total", -1, "%" G_GUINT64_FORMAT,
                (guint64)metrics.sender->send_errors);
        family(out, "geo_tracker_udp_frames_dropped_total", "counter",
                "Frames dropped because the send queue overflowed.");
        sample(out, "geo_tracker_udp_frames_dropped_total", -1, "%" G_GUINT64_FORMAT,
                (guint64)metrics.sender->frames_dropped);
    }

    return out;
}

bool metrics_start(MetricsServer &metrics, std::string &error) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(metrics.port));
    if (inet_pton(AF_INET, metrics.bind_address.c_str(), &addr.sin_addr) != 1) {
        error = "bad IPv4 address '" + metrics.bind_address + "'";
        return false;
    }

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::string("socket: ") + g_strerror(errno);
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        error = metrics.bind_address + ":" + std::to_string(metrics.port) + ": " + g_strerror(errno);
        close(fd);
        return false;
    }

    metrics.listen_fd = fd;
    metrics.started_us = g_get_monotonic_time();
    metrics.stopping = false;
    render_tick(&metrics);
    metrics.timer_id = g_timeout_add_seconds(RENDER_INTERVAL_SEC, render_tick, &metrics);
    metrics.worker = std::thread(serve, &metrics);

    LOG_INFO("METRICS", "serving Prometheus metrics at http://%s:%d/metrics",
            metrics.bind_address.c_str(), metrics.port);
    return true;
}

void metrics_stop(MetricsServer &metrics) {
    metrics.stopping = true;
    if (metrics.worker.joinable()) {
        metrics.worker.join();
    }
    if (metrics.timer_id != 0) {
        g_source_remove(metrics.timer_id);
        metrics.timer_id = 0;
    }
    if (metrics.listen_fd >= 0) {
        close(metrics.listen_fd);
        metrics.listen_fd = -1;
    }
}
