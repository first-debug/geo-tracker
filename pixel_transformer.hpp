#ifndef PIXEL_TRANSFORMER_H
#define PIXEL_TRANSFORMER_H

#include <opencv2/opencv.hpp>
#include <vector>
#include <array>
#include <optional>
#include <utility>

/*
* PixelGeoTransformer преобразует пиксельные координаты в географические.
*
* Географические координты должны быть в формате (lng, lat).
*
* Гомография считается не в градусах, а в локальной метрической системе
* (восток, север) с началом в центре калибровочных точек, поэтому порог
* RANSAC и ошибка репроекции измеряются в метрах.
*/
class PixelGeoTransformer {
public:
    struct ReprojectionStats {
        size_t points = 0;
        size_t inliers = 0;
        double median_m = 0.0;
        double p90_m = 0.0;
        double max_m = 0.0;
    };

private:
    std::array<double, 9> H_flat;
    bool is_valid = false;
    std::vector<cv::Point2d> src_points;
    std::vector<cv::Point2d> dst_points;

    // начало локальной системы координат и масштаб (метров на градус)
    double lng0 = 0.0, lat0 = 0.0;
    double m_per_deg_lng = 1.0, m_per_deg_lat = 1.0;

    ReprojectionStats stats;

    void calculate_homography(double ransac_threshold_m);
public:
    PixelGeoTransformer(const std::vector<cv::Point2d>& points_pixels,
                        const std::vector<cv::Point2d>& points_geo,
                        double ransac_threshold_m = 10.0);
    /**
     * @brief Преобразует одну пиксельную точку в географическую (lng, lat).
     * Не бросает исключений: возвращает std::nullopt, если точка уходит
     * в бесконечность или матрица не инициализирована.
     */
    std::optional<std::pair<double, double>> pixel_to_geo(double x_pixel, double y_pixel) const noexcept;
    /**
     * @brief Пакетное преобразование множества точек.
     * Точки, которые нельзя преобразовать, пропускаются.
     */
    std::vector<std::pair<double, double>> batch_pixel_to_geo(
        const std::vector<cv::Point2d>& pixels) const;
    // Ошибка на калибровочных точках, посчитанная при построении
    const ReprojectionStats& reprojection_stats() const;
    // Геттер для проверки состояния
    bool isValid() const;
};

#endif // PIXEL_TRANSFORMER_H
