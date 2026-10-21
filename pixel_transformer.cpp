#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include "pixel_transformer.hpp"

void PixelGeoTransformer::calculate_homography(double ransac_threshold_m) {
    if (src_points.size() < 4 || dst_points.size() < 4) {
        throw std::invalid_argument("Для вычисления матрицы гомографии требуется минимум 4 точки.");
    }
    if (src_points.size() != dst_points.size()) {
        throw std::invalid_argument("Количество пиксельных и географических точек должно совпадать.");
    }

    // Центр калибровочных точек — начало локальной системы координат.
    lng0 = 0.0;
    lat0 = 0.0;
    for (const auto& p : dst_points) {
        lng0 += p.x;
        lat0 += p.y;
    }
    lng0 /= dst_points.size();
    lat0 /= dst_points.size();

    // Длина градуса на эллипсоиде WGS84 для широты lat0.
    // На масштабе аэропорта (единицы км) погрешность такого приближения — миллиметры.
    double phi = lat0 * CV_PI / 180.0;
    m_per_deg_lat = 111132.92 - 559.82 * std::cos(2 * phi) + 1.175 * std::cos(4 * phi);
    m_per_deg_lng = 111412.84 * std::cos(phi) - 93.5 * std::cos(3 * phi);

    std::vector<cv::Point2d> dst_metric;
    dst_metric.reserve(dst_points.size());
    for (const auto& p : dst_points) {
        dst_metric.emplace_back((p.x - lng0) * m_per_deg_lng, (p.y - lat0) * m_per_deg_lat);
    }

    cv::Mat inliers_mask;
    cv::Mat H_mat = cv::findHomography(src_points, dst_metric, cv::RANSAC,
                                       ransac_threshold_m, inliers_mask);

    if (H_mat.empty()) {
        throw std::runtime_error("Не удалось вычислить матрицу гомографии.");
    }

    // Приводим к double (CV_64F) для сохранения точности координат
    cv::Mat H_double;
    H_mat.convertTo(H_double, CV_64F);

    // Разворачиваем матрицу 3x3 в плоский массив
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            H_flat[i * 3 + j] = H_double.at<double>(i, j);
        }
    }
    // Знак w и порог горизонта берутся по точкам, вошедшим в гомографию:
    // выброс за горизонтом не должен сдвигать порог.
    std::vector<double> calib_w;
    for (size_t i = 0; i < src_points.size(); ++i) {
        if (inliers_mask.empty() || inliers_mask.at<uchar>(static_cast<int>(i))) {
            calib_w.push_back(H_flat[6] * src_points[i].x + H_flat[7] * src_points[i].y + H_flat[8]);
        }
    }
    size_t positive = std::count_if(calib_w.begin(), calib_w.end(), [](double w) { return w > 0; });
    w_sign = positive * 2 >= calib_w.size() ? 1.0 : -1.0;
    double min_abs_w = std::numeric_limits<double>::infinity();
    for (double w : calib_w) {
        if (w * w_sign > 0) {
            min_abs_w = std::min(min_abs_w, w * w_sign);
        }
    }
    w_min = std::max(min_abs_w * kHorizonMargin, 1e-12);
    is_valid = true;

    // Ошибка репроекции на калибровочных точках, в метрах
    std::vector<double> errors;
    errors.reserve(src_points.size());
    for (size_t i = 0; i < src_points.size(); ++i) {
        auto geo = pixel_to_geo(src_points[i].x, src_points[i].y);
        if (!geo) {
            errors.push_back(std::numeric_limits<double>::infinity());
            continue;
        }
        double dx = (geo->first - dst_points[i].x) * m_per_deg_lng;
        double dy = (geo->second - dst_points[i].y) * m_per_deg_lat;
        errors.push_back(std::hypot(dx, dy));
    }
    std::sort(errors.begin(), errors.end());

    stats.points = errors.size();
    stats.inliers = inliers_mask.empty() ? errors.size() : cv::countNonZero(inliers_mask);
    stats.median_m = errors[errors.size() / 2];
    stats.p90_m = errors[std::min(errors.size() - 1, errors.size() * 9 / 10)];
    stats.max_m = errors.back();
}

PixelGeoTransformer::PixelGeoTransformer(const std::vector<cv::Point2d>& points_pixels,
                    const std::vector<cv::Point2d>& points_geo,
                    double ransac_threshold_m)
    : src_points(points_pixels), dst_points(points_geo) {
    calculate_homography(ransac_threshold_m);
}

std::optional<std::pair<double, double>> PixelGeoTransformer::pixel_to_geo(
        double x_pixel, double y_pixel) const noexcept {
    if (!is_valid) {
        return std::nullopt;
    }

    // H_flat layout:
    // [0] [1] [2]
    // [3] [4] [5]
    // [6] [7] [8]

    double w = H_flat[6] * x_pixel + H_flat[7] * y_pixel + H_flat[8];

    // Горизонт и всё, что над ним (знак w другой), плюс слишком далёкие точки
    if (w * w_sign < w_min) {
        return std::nullopt;
    }

    double inv_w = 1.0 / w;
    double east = (H_flat[0] * x_pixel + H_flat[1] * y_pixel + H_flat[2]) * inv_w;
    double north = (H_flat[3] * x_pixel + H_flat[4] * y_pixel + H_flat[5]) * inv_w;

    double lon = lng0 + east / m_per_deg_lng;
    double lat = lat0 + north / m_per_deg_lat;

    return std::make_pair(lon, lat);
}

std::vector<std::pair<double, double>> PixelGeoTransformer::batch_pixel_to_geo(
    const std::vector<cv::Point2d>& pixels) const {

    std::vector<std::pair<double, double>> result;
    result.reserve(pixels.size()); // Предварительное выделение памяти

    for (const auto& p : pixels) {
        if (auto geo = pixel_to_geo(p.x, p.y)) {
            result.push_back(*geo);
        }
    }
    return result;
}

const PixelGeoTransformer::ReprojectionStats& PixelGeoTransformer::reprojection_stats() const {
    return stats;
}

// Геттер для проверки состояния
bool PixelGeoTransformer::isValid() const { return is_valid; }
