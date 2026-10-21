#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "test.hpp"
#include "../points_sender.hpp"

namespace {

// Принимает датаграммы на 127.0.0.1 в отдельном потоке, чтобы при быстрой
// отправке большой пачки не переполнился буфер сокета.
class UdpReceiver {
public:
    UdpReceiver() {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
        port = ntohs(addr.sin_port);

        struct timeval tv {0, 20000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        worker = std::thread([this] { run(); });
    }

    ~UdpReceiver() {
        running = false;
        worker.join();
        close(fd);
    }

    // Ждёт, пока датаграммы перестанут приходить (quiet без новых), и возвращает все
    std::vector<std::string> wait_all(int quiet_ms = 200) {
        size_t last = size_t(-1);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(quiet_ms));
            std::lock_guard<std::mutex> lock(mutex);
            if (datagrams.size() == last) {
                return datagrams;
            }
            last = datagrams.size();
        }
    }

    uint16_t port = 0;

private:
    void run() {
        std::vector<char> buf(65536);
        while (running) {
            ssize_t n = recv(fd, buf.data(), buf.size(), 0);
            if (n >= 0) {
                std::lock_guard<std::mutex> lock(mutex);
                datagrams.emplace_back(buf.data(), static_cast<size_t>(n));
            }
        }
    }

    int fd = -1;
    std::atomic<bool> running{true};
    std::mutex mutex;
    std::vector<std::string> datagrams;
    std::thread worker;
};

std::vector<sheremetevo::Batch> parse_all(const std::vector<std::string> &datagrams) {
    std::vector<sheremetevo::Batch> batches;
    for (const std::string &d : datagrams) {
        sheremetevo::Batch b;
        if (!b.ParseFromString(d)) {
            test::fail(__FILE__, __LINE__, "datagram is not a valid Batch");
        }
        batches.push_back(b);
    }
    return batches;
}

double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Кадр с n объектами; поля объектов зависят от номера, чтобы проверять порядок
struct TestFrame {
    std::vector<future_t> objects;
    std::vector<std::string> names;
    points_message_t msg {};

    TestFrame(int camera_id, double timestamp, size_t n, size_t name_len = 8) {
        names.reserve(n);
        for (size_t i = 0; i < n; i++) {
            names.push_back(std::string(name_len, char('a' + i % 26)));
        }
        for (size_t i = 0; i < n; i++) {
            future_t f {};
            f.lat = 55.98 + i * 1e-6;
            f.lng = 37.41 + i * 1e-6;
            f.object_id = static_cast<int64_t>(i);
            f.name = names[i].c_str();
            f.confidence = 0.5;
            f.bbox = {int(i), int(i) + 1, int(i) + 10, int(i) + 20};
            objects.push_back(f);
        }
        msg.camera_id = camera_id;
        msg.timestamp = timestamp;
        msg.future = objects.data();
        msg.future_size = objects.size();
    }
};

void check_sequences(const std::vector<sheremetevo::Batch> &batches) {
    for (size_t i = 0; i < batches.size(); i++) {
        CHECK_EQ(batches[i].sequence(), uint64_t(i));
    }
}

// Интервал, который не истечёт за время теста: всё уходит при stop()
const int kLongInterval = 60000;

} // namespace

TEST("points_sender: frame fields are mapped to protobuf") {
    UdpReceiver rx;
    UdpConnection conn("127.0.0.1", rx.port);
    REQUIRE(conn.is_open());

    future_t objects[2] {};
    objects[0].lat = 55.9805123456789;
    objects[0].lng = 37.4146987654321;
    objects[0].object_id = -1;
    objects[0].name = "airplane";
    objects[0].confidence = 0.875;
    objects[0].bbox = {10, 20, 300, 140};
    objects[1].lat = 55.0;
    objects[1].lng = 37.0;
    objects[1].object_id = 1234567890123LL;
    objects[1].name = nullptr;
    objects[1].confidence = 0.25;
    objects[1].bbox = {-5, 0, 1727, 2751};
    points_message_t msg {3, 1761057083.125, objects, 2};

    double before = now_sec();
    {
        PointsSender sender(conn, 0, 1400);
        sender.add_frame(msg);
        // stop() в деструкторе дожидается отправки
    }
    double after = now_sec();

    auto batches = parse_all(rx.wait_all());
    REQUIRE_EQ(batches.size(), size_t(1));
    const sheremetevo::Batch &b = batches[0];
    CHECK_EQ(b.sequence(), uint64_t(0));
    CHECK_LE(before, b.sent_at());
    CHECK_LE(b.sent_at(), after);

    REQUIRE_EQ(b.frames_size(), 1);
    const sheremetevo::Frame &f = b.frames(0);
    CHECK_EQ(f.camera_id(), 3u);
    CHECK_NEAR(f.timestamp(), 1761057083.125, 1e-9);
    REQUIRE_EQ(f.detections_size(), 2);

    const sheremetevo::Detection &d0 = f.detections(0);
    CHECK_NEAR(d0.lat(), 55.9805123456789, 1e-13);
    CHECK_NEAR(d0.lng(), 37.4146987654321, 1e-13);
    CHECK_EQ(d0.object_id(), int64_t(-1));
    CHECK_EQ(d0.name(), std::string("airplane"));
    CHECK_NEAR(d0.confidence(), 0.875, 1e-7);
    CHECK_EQ(d0.bbox().x1(), 10);
    CHECK_EQ(d0.bbox().y1(), 20);
    CHECK_EQ(d0.bbox().x2(), 300);
    CHECK_EQ(d0.bbox().y2(), 140);

    const sheremetevo::Detection &d1 = f.detections(1);
    CHECK_EQ(d1.object_id(), int64_t(1234567890123LL));
    CHECK_EQ(d1.name(), std::string(""));
    CHECK_EQ(d1.bbox().x1(), -5);
    CHECK_EQ(d1.bbox().y2(), 2751);
}

TEST("points_sender: frame without objects is sent") {
    UdpReceiver rx;
    UdpConnection conn("127.0.0.1", rx.port);
    {
        PointsSender sender(conn, 0, 1400);
        TestFrame empty(1, 100.0, 0);
        sender.add_frame(empty.msg);
    }
    auto batches = parse_all(rx.wait_all());
    REQUIRE_EQ(batches.size(), size_t(1));
    REQUIRE_EQ(batches[0].frames_size(), 1);
    CHECK_EQ(batches[0].frames(0).camera_id(), 1u);
    CHECK_EQ(batches[0].frames(0).detections_size(), 0);
}

TEST("points_sender: frames are packed into datagrams within max-datagram-bytes") {
    const size_t max_bytes = 1400;
    const int n_frames = 60;
    UdpReceiver rx;
    UdpConnection conn("127.0.0.1", rx.port);
    {
        PointsSender sender(conn, kLongInterval, max_bytes);
        for (int i = 0; i < n_frames; i++) {
            TestFrame f(i % 4, 1000.0 + i, 3);
            sender.add_frame(f.msg);
        }
    }

    auto datagrams = rx.wait_all();
    auto batches = parse_all(datagrams);
    // 60 кадров по ~200 байт не помещаются в одну датаграмму,
    // но и по одному кадру на датаграмму быть не должно
    CHECK_LT(size_t(1), batches.size());
    CHECK_LT(batches.size(), size_t(n_frames / 2));
    for (const std::string &d : datagrams) {
        CHECK_LE(d.size(), max_bytes);
    }
    check_sequences(batches);

    // все кадры дошли, в исходном порядке и целиком
    int next = 0;
    for (const auto &b : batches) {
        for (const auto &f : b.frames()) {
            CHECK_NEAR(f.timestamp(), 1000.0 + next, 1e-9);
            CHECK_EQ(f.camera_id(), uint32_t(next % 4));
            CHECK_EQ(f.detections_size(), 3);
            next++;
        }
    }
    CHECK_EQ(next, n_frames);
}

TEST("points_sender: large frame is split by objects across datagrams") {
    const size_t max_bytes = 1400;
    const size_t n_objects = 200;
    UdpReceiver rx;
    UdpConnection conn("127.0.0.1", rx.port);
    {
        PointsSender sender(conn, kLongInterval, max_bytes);
        TestFrame small_before(0, 1.0, 1);
        TestFrame big(2, 2.0, n_objects);
        TestFrame small_after(1, 3.0, 1);
        sender.add_frame(small_before.msg);
        sender.add_frame(big.msg);
        sender.add_frame(small_after.msg);
    }

    auto datagrams = rx.wait_all();
    auto batches = parse_all(datagrams);
    CHECK_LT(size_t(3), batches.size());
    for (const std::string &d : datagrams) {
        CHECK_LE(d.size(), max_bytes);
    }
    check_sequences(batches);

    // порядок: маленький кадр, части большого, маленький кадр
    std::vector<const sheremetevo::Frame *> frames;
    for (const auto &b : batches) {
        for (const auto &f : b.frames()) {
            frames.push_back(&f);
        }
    }
    REQUIRE(frames.size() >= 4);
    CHECK_NEAR(frames.front()->timestamp(), 1.0, 1e-9);
    CHECK_NEAR(frames.back()->timestamp(), 3.0, 1e-9);

    // части большого кадра: тот же camera_id и timestamp, объекты по порядку
    int64_t next_id = 0;
    for (size_t i = 1; i + 1 < frames.size(); i++) {
        CHECK_EQ(frames[i]->camera_id(), 2u);
        CHECK_NEAR(frames[i]->timestamp(), 2.0, 1e-9);
        CHECK(frames[i]->detections_size() > 0);
        for (const auto &d : frames[i]->detections()) {
            CHECK_EQ(d.object_id(), next_id);
            next_id++;
        }
    }
    CHECK_EQ(next_id, int64_t(n_objects));
}

TEST("points_sender: split works at the smallest allowed datagram size") {
    const size_t max_bytes = 256;
    UdpReceiver rx;
    UdpConnection conn("127.0.0.1", rx.port);
    {
        PointsSender sender(conn, kLongInterval, max_bytes);
        for (int i = 0; i < 5; i++) {
            TestFrame f(i, i, 7, 20);
            sender.add_frame(f.msg);
        }
    }

    auto datagrams = rx.wait_all();
    size_t detections = 0;
    for (const std::string &d : datagrams) {
        CHECK_LE(d.size(), max_bytes);
    }
    auto batches = parse_all(datagrams);
    check_sequences(batches);
    for (const auto &b : batches) {
        for (const auto &f : b.frames()) {
            detections += f.detections_size();
        }
    }
    CHECK_EQ(detections, size_t(5 * 7));
}

TEST("points_sender: object that cannot fit any datagram is dropped, others are sent") {
    const size_t max_bytes = 256;
    UdpReceiver rx;
    UdpConnection conn("127.0.0.1", rx.port);
    {
        PointsSender sender(conn, kLongInterval, max_bytes);
        TestFrame f(0, 5.0, 3);
        f.names[1] = std::string(400, 'x');
        f.objects[1].name = f.names[1].c_str();
        sender.add_frame(f.msg);
    }

    auto datagrams = rx.wait_all();
    for (const std::string &d : datagrams) {
        CHECK_LE(d.size(), max_bytes);
    }
    std::vector<int64_t> ids;
    for (const auto &b : parse_all(datagrams)) {
        for (const auto &f : b.frames()) {
            for (const auto &d : f.detections()) {
                ids.push_back(d.object_id());
            }
        }
    }
    REQUIRE_EQ(ids.size(), size_t(2));
    CHECK_EQ(ids[0], int64_t(0));
    CHECK_EQ(ids[1], int64_t(2));
}

TEST("points_sender: empty Batch is sent every interval as a heartbeat") {
    UdpReceiver rx;
    UdpConnection conn("127.0.0.1", rx.port);
    {
        PointsSender sender(conn, 50, 1400);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    auto batches = parse_all(rx.wait_all());
    // ~8 интервалов и ещё одна пачка при остановке; запас на медленную машину
    CHECK_LE(size_t(3), batches.size());
    CHECK_LE(batches.size(), size_t(12));
    check_sequences(batches);
    for (const auto &b : batches) {
        CHECK_EQ(b.frames_size(), 0);
    }
}

TEST("points_sender: with interval 0 nothing is sent without frames") {
    UdpReceiver rx;
    UdpConnection conn("127.0.0.1", rx.port);
    {
        PointsSender sender(conn, 0, 1400);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK_EQ(rx.wait_all().size(), size_t(0));
}

TEST("points_sender: frames after stop() are ignored, stop() is idempotent") {
    UdpReceiver rx;
    UdpConnection conn("127.0.0.1", rx.port);
    PointsSender sender(conn, 0, 1400);
    TestFrame f(0, 1.0, 1);
    sender.add_frame(f.msg);
    sender.stop();
    sender.add_frame(f.msg);
    sender.stop();

    auto batches = parse_all(rx.wait_all());
    REQUIRE_EQ(batches.size(), size_t(1));
    CHECK_EQ(batches[0].frames_size(), 1);
}

TEST("points_sender: queue is bounded when the sender cannot keep up") {
    UdpReceiver rx;
    UdpConnection conn("127.0.0.1", rx.port);
    const int n_frames = 10050;  // предел очереди — 10000 кадров
    {
        PointsSender sender(conn, kLongInterval, 65507);
        TestFrame f(0, 1.0, 0);
        for (int i = 0; i < n_frames; i++) {
            f.msg.timestamp = i;
            sender.add_frame(f.msg);
        }
    }

    size_t frames = 0;
    double last_ts = -1;
    for (const auto &b : parse_all(rx.wait_all())) {
        for (const auto &fr : b.frames()) {
            frames++;
            last_ts = fr.timestamp();
        }
    }
    CHECK_EQ(frames, size_t(10000));
    // отбрасываются новые кадры, а не старые
    CHECK_NEAR(last_ts, 9999, 1e-9);
}
