#ifndef GEO_ZONE_H
#define GEO_ZONE_H

#include <string>
#include <vector>

#include "opencv2/core/types.hpp"

/*
 * GeoZone — многоугольник на земле, в котором ожидаются объекты
 * (перрон, рулёжки). Точки вне зоны не отправляются получателю.
 *
 * Вершины задаются в (lng, lat), проверка идёт в локальной метрической
 * системе (восток, север) с началом в центре вершин. Пустая зона
 * (не загружена) пропускает все точки.
 */
class GeoZone {
public:
    GeoZone() = default;
    // Бросает std::invalid_argument, если вершин меньше 3
    explicit GeoZone(const std::vector<cv::Point2d> &vertices_geo);

    bool empty() const { return polygon.empty(); }
    // Точка (lng, lat) внутри зоны или на её границе; для пустой зоны всегда true
    bool contains(double lng, double lat) const;

    size_t vertex_count() const { return polygon.size(); }
    // Площадь зоны, м²
    double area_m2() const;

private:
    double lng0 = 0.0, lat0 = 0.0;
    double m_per_deg_lng = 1.0, m_per_deg_lat = 1.0;
    std::vector<cv::Point2f> polygon;
};

/*
 * Читает CSV с вершинами зоны. Строки: lng,lat, по порядку обхода контура.
 * Пустые строки, строки с '#' и строка заголовка пропускаются. Нужно минимум 3 вершины.
 */
bool load_geo_zone(const std::string &path, GeoZone &zone, std::string &error);

#endif // GEO_ZONE_H
