#include <cmath>
#include <stdexcept>
#include <vector>

#include "test.hpp"
#include "../pixel_transformer.hpp"

namespace {

// Точка отсчёта для синтетических данных — район Шереметьево
const double kLng0 = 37.4146;
const double kLat0 = 55.9805;

// Длина градуса на WGS84 (те же ряды, что в pixel_transformer.cpp)
double m_per_deg_lat(double lat) {
    double phi = lat * CV_PI / 180.0;
    return 111132.92 - 559.82 * std::cos(2 * phi) + 1.175 * std::cos(4 * phi);
}

double m_per_deg_lng(double lat) {
    double phi = lat * CV_PI / 180.0;
    return 111412.84 * std::cos(phi) - 93.5 * std::cos(3 * phi);
}

// Расстояние между двумя (lng, lat) в метрах (на масштабе сотен метров
// локальное приближение точнее сантиметра)
double distance_m(double lng1, double lat1, double lng2, double lat2) {
    double lat = (lat1 + lat2) / 2;
    return std::hypot((lng1 - lng2) * m_per_deg_lng(lat), (lat1 - lat2) * m_per_deg_lat(lat));
}

// Эталонная перспективная проекция пиксель -> (восток, север) в метрах от
// (kLng0, kLat0), как у камеры, смотрящей наклонно на плоское поле.
const double kH[9] = {
    0.25,  0.02,   -150.0,
    0.01, -0.30,    400.0,
    0.0,   0.0004,  1.0,
};

cv::Point2d true_geo(double x, double y) {
    double w = kH[6] * x + kH[7] * y + kH[8];
    double east = (kH[0] * x + kH[1] * y + kH[2]) / w;
    double north = (kH[3] * x + kH[4] * y + kH[5]) / w;
    return {kLng0 + east / m_per_deg_lng(kLat0), kLat0 + north / m_per_deg_lat(kLat0)};
}

// Сетка калибровочных точек по кадру 1728x2752
void make_grid(std::vector<cv::Point2d> &pixels, std::vector<cv::Point2d> &geo) {
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            double x = 100 + i * 380;
            double y = 200 + j * 600;
            pixels.emplace_back(x, y);
            geo.push_back(true_geo(x, y));
        }
    }
}

// Сдвигает точку на east/north метров
cv::Point2d shift(const cv::Point2d &p, double east, double north) {
    return {p.x + east / m_per_deg_lng(p.y), p.y + north / m_per_deg_lat(p.y)};
}

} // namespace

TEST("pixel_transformer: exact data is reproduced to centimetres") {
    std::vector<cv::Point2d> pixels, geo;
    make_grid(pixels, geo);

    PixelGeoTransformer t(pixels, geo);
    REQUIRE(t.isValid());

    // калибровочные точки
    for (size_t i = 0; i < pixels.size(); i++) {
        auto g = t.pixel_to_geo(pixels[i].x, pixels[i].y);
        REQUIRE(g.has_value());
        CHECK_LT(distance_m(g->first, g->second, geo[i].x, geo[i].y), 0.01);
    }

    // точки между узлами сетки
    for (double x : {333.0, 1001.5, 1600.0}) {
        for (double y : {555.0, 1777.0, 2600.0}) {
            auto g = t.pixel_to_geo(x, y);
            REQUIRE(g.has_value());
            cv::Point2d expected = true_geo(x, y);
            CHECK_LT(distance_m(g->first, g->second, expected.x, expected.y), 0.01);
        }
    }

    const auto &stats = t.reprojection_stats();
    CHECK_EQ(stats.points, pixels.size());
    CHECK_EQ(stats.inliers, pixels.size());
    CHECK_LT(stats.max_m, 0.01);
}

TEST("pixel_transformer: result is (lng, lat), not (lat, lng)") {
    std::vector<cv::Point2d> pixels, geo;
    make_grid(pixels, geo);
    PixelGeoTransformer t(pixels, geo);

    auto g = t.pixel_to_geo(pixels[7].x, pixels[7].y);
    REQUIRE(g.has_value());
    CHECK_NEAR(g->first, geo[7].x, 1e-6);
    CHECK_NEAR(g->second, geo[7].y, 1e-6);
}

TEST("pixel_transformer: RANSAC threshold is in metres and rejects outliers") {
    std::vector<cv::Point2d> pixels, geo;
    make_grid(pixels, geo);
    std::vector<cv::Point2d> clean_geo = geo;

    // Три грубые ошибки по 50 м. Если бы порог 10 был в градусах,
    // а не в метрах, все точки считались бы хорошими и сдвигали ответ.
    geo[3] = shift(geo[3], 50, 0);
    geo[12] = shift(geo[12], 0, -50);
    geo[20] = shift(geo[20], 35, 35);

    PixelGeoTransformer t(pixels, geo, 10.0);
    const auto &stats = t.reprojection_stats();
    CHECK_EQ(stats.points, pixels.size());
    CHECK_EQ(stats.inliers, pixels.size() - 3);
    CHECK_LT(stats.median_m, 0.01);
    // статистика считается по всем точкам, включая выбросы
    CHECK_LT(45.0, stats.max_m);

    // ответ строится по хорошим точкам
    for (size_t i : {3, 12, 20}) {
        auto g = t.pixel_to_geo(pixels[i].x, pixels[i].y);
        REQUIRE(g.has_value());
        CHECK_LT(distance_m(g->first, g->second, clean_geo[i].x, clean_geo[i].y), 0.01);
    }
}

TEST("pixel_transformer: noisy points stay inliers under a metre-scale threshold") {
    std::vector<cv::Point2d> pixels, geo;
    make_grid(pixels, geo);

    // детерминированный «шум» до ±1.5 м
    for (size_t i = 0; i < geo.size(); i++) {
        double east = 1.5 * std::sin(i * 1.7);
        double north = 1.5 * std::cos(i * 2.3);
        geo[i] = shift(geo[i], east, north);
    }

    PixelGeoTransformer t(pixels, geo, 10.0);
    const auto &stats = t.reprojection_stats();
    CHECK_EQ(stats.inliers, pixels.size());
    CHECK_LT(stats.median_m, 3.0);
    CHECK_LE(stats.median_m, stats.p90_m);
    CHECK_LE(stats.p90_m, stats.max_m);

    // с порогом в сантиметры часть точек становится выбросами
    PixelGeoTransformer strict(pixels, geo, 0.05);
    CHECK_LT(strict.reprojection_stats().inliers, pixels.size());
}

TEST("pixel_transformer: fewer than 4 points is an error") {
    std::vector<cv::Point2d> pixels = {{0, 0}, {100, 0}, {100, 100}};
    std::vector<cv::Point2d> geo = {true_geo(0, 0), true_geo(100, 0), true_geo(100, 100)};
    CHECK_THROWS_AS(PixelGeoTransformer(pixels, geo), std::invalid_argument);
}

TEST("pixel_transformer: different number of pixel and geo points is an error") {
    std::vector<cv::Point2d> pixels, geo;
    make_grid(pixels, geo);
    geo.pop_back();
    CHECK_THROWS_AS(PixelGeoTransformer(pixels, geo), std::invalid_argument);
}

TEST("pixel_transformer: degenerate (collinear) points are an error") {
    std::vector<cv::Point2d> pixels, geo;
    for (int i = 0; i < 6; i++) {
        pixels.emplace_back(100 + i * 100, 500);
        geo.push_back(true_geo(100 + i * 100, 500));
    }
    CHECK_THROWS_AS(PixelGeoTransformer(pixels, geo), std::runtime_error);
}

// ---------------------------------------------------------------------------
// горизонт
// ---------------------------------------------------------------------------

// У эталонной камеры w = 0.0004·y + 1: горизонт на y = -2500, над ним
// (y < -2500) w < 0 и точка «отражается» за спину камеры. На калибровочной
// сетке (y = 200..2600) наименьший w = 1.08, порог 0.108 -> y ≈ -2230.

TEST("pixel_transformer: far but valid points below the horizon are kept") {
    std::vector<cv::Point2d> pixels, geo;
    make_grid(pixels, geo);
    PixelGeoTransformer t(pixels, geo);

    // ~5 км, в 9 раз дальше калибровочной зоны: ещё принимается
    auto g = t.pixel_to_geo(800, -2000);
    REQUIRE(g.has_value());
    cv::Point2d expected = true_geo(800, -2000);
    CHECK_LT(distance_m(g->first, g->second, expected.x, expected.y), 1.0);
}

TEST("pixel_transformer: points at and above the horizon are rejected") {
    std::vector<cv::Point2d> pixels, geo;
    make_grid(pixels, geo);
    PixelGeoTransformer t(pixels, geo);

    // у горизонта (сотни км), на нём (деление на ноль) и над ним:
    // -2510 и -3000 без фильтра дают «правдоподобные» точки за камерой
    for (double y : {-2300.0, -2490.0, -2500.0, -2510.0, -3000.0, -10000.0}) {
        CHECK(!t.pixel_to_geo(800, y).has_value());
    }
    CHECK(t.batch_pixel_to_geo({{800, 200}, {800, -3000}, {800, 2600}}).size() == 2);
}

TEST("pixel_transformer: horizon side is learnt from calibration, not assumed") {
    // Камера, у которой после нормализации w на земле отрицательный:
    // w' = 1 - 0.001·y, горизонт на y = 1000, земля ниже (y > 1000)
    const double kH2[9] = {
        0.25,  0.02,  -150.0,
        0.01, -0.30,   400.0,
        0.0,  -0.001,  1.0,
    };
    auto geo_of = [&](double x, double y) {
        double w = kH2[6] * x + kH2[7] * y + kH2[8];
        double east = (kH2[0] * x + kH2[1] * y + kH2[2]) / w;
        double north = (kH2[3] * x + kH2[4] * y + kH2[5]) / w;
        return cv::Point2d(kLng0 + east / m_per_deg_lng(kLat0), kLat0 + north / m_per_deg_lat(kLat0));
    };
    std::vector<cv::Point2d> pixels, geo;
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            double x = 100 + i * 380, y = 1300 + j * 350;
            pixels.emplace_back(x, y);
            geo.push_back(geo_of(x, y));
        }
    }
    PixelGeoTransformer t(pixels, geo);
    CHECK_LT(t.reprojection_stats().max_m, 0.01);

    // земля: калибровочная точка и точка дальше калибровки
    CHECK(t.pixel_to_geo(800, 1300).has_value());
    CHECK(t.pixel_to_geo(800, 1100).has_value());
    // горизонт и небо
    CHECK(!t.pixel_to_geo(800, 1000).has_value());
    CHECK(!t.pixel_to_geo(800, 900).has_value());
    CHECK(!t.pixel_to_geo(800, 0).has_value());
}

TEST("pixel_transformer: outlier beyond the horizon does not break the filter") {
    std::vector<cv::Point2d> pixels, geo;
    make_grid(pixels, geo);
    // ошибочная калибровочная точка в «небе» с координатами из рабочей зоны
    pixels.emplace_back(800, -3000);
    geo.push_back(true_geo(800, 1000));

    PixelGeoTransformer t(pixels, geo, 10.0);
    CHECK_EQ(t.reprojection_stats().inliers, pixels.size() - 1);
    CHECK(t.pixel_to_geo(800, 200).has_value());
    CHECK(!t.pixel_to_geo(800, -3000).has_value());
}
