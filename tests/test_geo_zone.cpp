#include <stdlib.h>
#include <unistd.h>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "test.hpp"
#include "../geo_zone.hpp"
#include "../pixel_transformer.hpp"

#ifndef TEST_SRC_DIR
#define TEST_SRC_DIR "."
#endif

namespace {

const double kLng0 = 37.4146;
const double kLat0 = 55.9805;

double m_per_deg_lat(double lat) {
    double phi = lat * CV_PI / 180.0;
    return 111132.92 - 559.82 * std::cos(2 * phi) + 1.175 * std::cos(4 * phi);
}

double m_per_deg_lng(double lat) {
    double phi = lat * CV_PI / 180.0;
    return 111412.84 * std::cos(phi) - 93.5 * std::cos(3 * phi);
}

// Точка в east/north метрах от (kLng0, kLat0), как (lng, lat)
cv::Point2d at(double east, double north) {
    return {kLng0 + east / m_per_deg_lng(kLat0), kLat0 + north / m_per_deg_lat(kLat0)};
}

// Г-образная (невыпуклая) зона: квадрат 400x400 м без правого верхнего угла 200x200
GeoZone l_zone() {
    return GeoZone({at(0, 0), at(400, 0), at(400, 200), at(200, 200), at(200, 400), at(0, 400)});
}

bool inside(const GeoZone &z, double east, double north) {
    cv::Point2d p = at(east, north);
    return z.contains(p.x, p.y);
}

std::string write_temp(const std::string &content) {
    char tmpl[] = "/tmp/sheremetevo-zone-XXXXXX";
    int fd = mkstemp(tmpl);
    close(fd);
    std::ofstream(tmpl) << content;
    return tmpl;
}

struct Loaded {
    bool ok;
    GeoZone zone;
    std::string error;
};

Loaded load(const std::string &content) {
    std::string path = write_temp(content);
    Loaded r;
    r.ok = load_geo_zone(path, r.zone, r.error);
    unlink(path.c_str());
    return r;
}

} // namespace

TEST("geo_zone: empty zone lets every point through") {
    GeoZone z;
    CHECK(z.empty());
    CHECK(z.contains(0, 0));
    CHECK(z.contains(kLng0, kLat0));
}

TEST("geo_zone: points inside and outside a concave polygon") {
    GeoZone z = l_zone();
    CHECK(!z.empty());
    CHECK_EQ(z.vertex_count(), size_t(6));
    // 0.01%: масштаб градуса берётся в центре зоны, а не в kLat0
    CHECK_NEAR(z.area_m2(), 400.0 * 400 - 200.0 * 200, 20.0);

    CHECK(inside(z, 100, 100));
    CHECK(inside(z, 300, 100));
    CHECK(inside(z, 100, 300));
    // вырезанный угол внутри описанного прямоугольника
    CHECK(!inside(z, 300, 300));
    CHECK(!inside(z, -1, 100));
    CHECK(!inside(z, 100, 401));
    CHECK(!inside(z, 5000, 5000));
}

TEST("geo_zone: boundary is metre-accurate and counts as inside") {
    GeoZone z = l_zone();
    CHECK(inside(z, 0, 200));
    CHECK(inside(z, 0.5, 200));
    CHECK(!inside(z, -0.5, 200));
    CHECK(inside(z, 399.5, 100));
    CHECK(!inside(z, 400.5, 100));
}

TEST("geo_zone: far away and invalid coordinates are outside") {
    GeoZone z = l_zone();
    CHECK(!z.contains(kLng0 + 90, kLat0));
    CHECK(!z.contains(403406.0, kLat0));
    CHECK(!z.contains(NAN, kLat0));
    CHECK(!z.contains(kLng0, INFINITY));
}

TEST("geo_zone: fewer than 3 vertices is an error") {
    CHECK_THROWS_AS(GeoZone({at(0, 0), at(100, 0)}), std::invalid_argument);
}

TEST("geo_zone csv: header, comments, blank lines, CRLF and spaces") {
    Loaded r = load(
        "# зона\r\n"
        "lng,lat\r\n"
        "\r\n"
        "37.4100, 55.9790\r\n"
        "  37.4120 ,55.9790  # комментарий\r\n"
        "37.4120,55.9810\r\n");
    REQUIRE(r.ok);
    CHECK_EQ(r.zone.vertex_count(), size_t(3));
    CHECK(r.zone.contains(37.4115, 55.9795));
    CHECK(!r.zone.contains(37.4105, 55.9805));
}

TEST("geo_zone csv: errors") {
    GeoZone z;
    std::string error;
    CHECK(!load_geo_zone("/nonexistent/zone.csv", z, error));
    CHECK_CONTAINS(error, "cannot open");

    Loaded two = load("37.41,55.97\n37.42,55.97\n");
    CHECK(!two.ok);
    CHECK_CONTAINS(two.error, "at least 3");

    Loaded bad = load("37.41,55.97\n37.42,55.97\n37.42\n");
    CHECK(!bad.ok);
    CHECK_CONTAINS(bad.error, ":3:");

    Loaded extra = load("37.41,55.97,10\n37.42,55.97\n37.42,55.98\n");
    CHECK(!extra.ok);
    CHECK_CONTAINS(extra.error, ":1:");

    // перепутаны lat и lng не ловятся, но широта за 90 — ошибка
    Loaded range = load("37.41,55.97\n37.42,95.0\n37.42,55.98\n");
    CHECK(!range.ok);
    CHECK_CONTAINS(range.error, ":2:");

    Loaded late_header = load("37.41,55.97\nlng,lat\n37.42,55.97\n37.42,55.98\n");
    CHECK(!late_header.ok);
    CHECK_CONTAINS(late_header.error, ":2:");
}

TEST("geo_zone csv: configs/zone.csv.example loads") {
    GeoZone z;
    std::string error;
    bool ok = load_geo_zone(std::string(TEST_SRC_DIR) + "/configs/zone.csv.example", z, error);
    CHECK_EQ(error, std::string(""));
    REQUIRE(ok);
    CHECK_EQ(z.vertex_count(), size_t(6));
}

TEST("geo_zone: horizon filter keeps reflected points out even if they land in the zone") {
    // Эталонная камера из test_pixel_transformer: горизонт на y = -2500,
    // точка над горизонтом отражается за спину камеры.
    const double kH[9] = {0.25, 0.02, -150.0, 0.01, -0.30, 400.0, 0.0, 0.0004, 1.0};
    auto geo_of = [&](double x, double y) {
        double w = kH[6] * x + kH[7] * y + kH[8];
        return at((kH[0] * x + kH[1] * y + kH[2]) / w, (kH[3] * x + kH[4] * y + kH[5]) / w);
    };
    std::vector<cv::Point2d> pixels, geo;
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            pixels.emplace_back(100 + i * 380, 200 + j * 600);
            geo.push_back(geo_of(pixels.back().x, pixels.back().y));
        }
    }
    PixelGeoTransformer t(pixels, geo);

    // Зона — большой квадрат вокруг камеры, отражённая точка в неё попадает
    cv::Point2d reflected = geo_of(800, -3000);
    GeoZone z({at(-20000, -20000), at(20000, -20000), at(20000, 20000), at(-20000, 20000)});
    CHECK(z.contains(reflected.x, reflected.y));
    // но pixel_to_geo её уже не возвращает
    CHECK(!t.pixel_to_geo(800, -3000).has_value());
    auto ground = t.pixel_to_geo(800, 1000);
    REQUIRE(ground.has_value());
    CHECK(z.contains(ground->first, ground->second));
}
