#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "geo_zone.hpp"

GeoZone::GeoZone(const std::vector<cv::Point2d> &vertices_geo) {
    if (vertices_geo.size() < 3) {
        throw std::invalid_argument("Для зоны требуется минимум 3 вершины.");
    }

    for (const auto &p : vertices_geo) {
        lng0 += p.x;
        lat0 += p.y;
    }
    lng0 /= vertices_geo.size();
    lat0 /= vertices_geo.size();

    // Длина градуса на WGS84, как в pixel_transformer.cpp
    double phi = lat0 * CV_PI / 180.0;
    m_per_deg_lat = 111132.92 - 559.82 * std::cos(2 * phi) + 1.175 * std::cos(4 * phi);
    m_per_deg_lng = 111412.84 * std::cos(phi) - 93.5 * std::cos(3 * phi);

    // float в метрах от центра: на масштабе аэропорта точность — миллиметры
    for (const auto &p : vertices_geo) {
        polygon.emplace_back(static_cast<float>((p.x - lng0) * m_per_deg_lng),
                             static_cast<float>((p.y - lat0) * m_per_deg_lat));
    }
}

bool GeoZone::contains(double lng, double lat) const {
    if (polygon.empty()) {
        return true;
    }
    cv::Point2f p(static_cast<float>((lng - lng0) * m_per_deg_lng),
                  static_cast<float>((lat - lat0) * m_per_deg_lat));
    // точки за тысячи км (float теряет точность) заведомо вне зоны
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > 1e6 || std::abs(p.y) > 1e6) {
        return false;
    }
    return cv::pointPolygonTest(polygon, p, false) >= 0;
}

double GeoZone::area_m2() const {
    return polygon.empty() ? 0.0 : cv::contourArea(polygon);
}

bool load_geo_zone(const std::string &path, GeoZone &zone, std::string &error) {
    std::ifstream file(path);
    if (!file) {
        error = path + ": cannot open file";
        return false;
    }

    std::vector<cv::Point2d> vertices;
    std::string line;
    for (int line_num = 1; std::getline(file, line); line_num++) {
        size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }

        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream fields(line);
        double lng, lat;
        std::string extra;
        if (!(fields >> lng >> lat) || (fields >> extra)) {
            // заголовок допустим только первой значимой строкой
            if (vertices.empty() && line.find("lng") != std::string::npos) {
                continue;
            }
            error = path + ":" + std::to_string(line_num) + ": expected lng,lat";
            return false;
        }
        if (lng < -180 || lng > 180 || lat < -90 || lat > 90) {
            error = path + ":" + std::to_string(line_num) + ": lng must be in -180..180, lat in -90..90";
            return false;
        }
        vertices.emplace_back(lng, lat);
    }

    if (vertices.size() < 3) {
        error = path + ": need at least 3 vertices, got " + std::to_string(vertices.size());
        return false;
    }
    zone = GeoZone(vertices);
    return true;
}
