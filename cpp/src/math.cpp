#include "robrun/math.h"

#include <algorithm>
#include <cmath>

namespace robrun {
namespace {

bool finite_vec(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

float length(Vec3 v) { return std::sqrt(dot(v, v)); }

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

}  // namespace

std::optional<Vec3> normalize(Vec3 v) {
    if (!finite_vec(v)) return std::nullopt;
    float len = length(v);
    if (!(len > 1e-6f) || !std::isfinite(len)) return std::nullopt;
    return Vec3{v.x / len, v.y / len, v.z / len};
}

std::optional<Projection> world_to_screen(const Camera& cam, Vec3 world, Viewport vp) {
    if (!cam.valid || !finite_vec(world) || !finite_vec(cam.pos)) return std::nullopt;
    if (vp.width <= 0 || vp.height <= 0) return std::nullopt;
    if (!std::isfinite(cam.fov_deg) || cam.fov_deg <= 1.0f || cam.fov_deg >= 179.0f)
        return std::nullopt;

    auto fwd = normalize(cam.fwd);
    if (!fwd) return std::nullopt;

    // Базис камеры: предпочитаем собственный right/up (+0x48/+0x54),
    // если они правдоподобны (длина 0.5..1.5). Иначе выводим из world up —
    // как game_w2s в референсе. Это критично при наклоне камеры (roll).
    Vec3 right = cam.right, up = cam.up;
    float rl = finite_vec(right) ? length(right) : 0.0f;
    float ul = finite_vec(up) ? length(up) : 0.0f;
    if (!(rl >= 0.5f && rl <= 1.5f && ul >= 0.5f && ul <= 1.5f)) {
        // right = up_world x fwd (Unity: Y-up)
        right = {1.0f * fwd->z - 0.0f * fwd->y,
                 0.0f * fwd->x - 0.0f * fwd->z,
                 0.0f * fwd->y - 1.0f * fwd->x};
        auto rn = normalize(right);
        if (!rn) {
            right = {1.0f, 0.0f, 0.0f};
            rn = normalize(right);
            if (!rn) return std::nullopt;
        }
        right = *rn;
        // up = fwd x right
        up = {fwd->y * right.z - fwd->z * right.y,
              fwd->z * right.x - fwd->x * right.z,
              fwd->x * right.y - fwd->y * right.x};
        auto un = normalize(up);
        if (!un) return std::nullopt;
        up = *un;
    } else {
        auto rn = normalize(right);
        auto un = normalize(up);
        if (!rn || !un) return std::nullopt;
        right = *rn;
        up = *un;
    }

    Vec3 d = sub(world, cam.pos);
    float cx = dot(d, right);
    float cy = dot(d, up);
    float cz = dot(d, *fwd);
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cz)) return std::nullopt;
    if (cz <= 0.05f) return std::nullopt;  // за камерой

    float aspect = static_cast<float>(vp.width) / static_cast<float>(vp.height);
    float tan_half = std::tan(cam.fov_deg * 3.14159265358979323846f / 360.0f);
    if (!std::isfinite(tan_half) || std::fabs(tan_half) < 1e-6f) return std::nullopt;

    float ndc_x = (cx / cz) / (tan_half * aspect);
    float ndc_y = (cy / cz) / tan_half;
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) return std::nullopt;

    Projection out;
    out.point.x = (ndc_x * 0.5f + 0.5f) * static_cast<float>(vp.width);
    out.point.y = (0.5f - ndc_y * 0.5f) * static_cast<float>(vp.height);
    out.point.depth = cz;
    out.on_screen = (ndc_x > -1.2f && ndc_x < 1.2f && ndc_y > -1.2f && ndc_y < 1.2f);
    return out;
}

std::optional<float> distance(Vec3 a, Vec3 b) {
    if (!finite_vec(a) || !finite_vec(b)) return std::nullopt;
    float d = length(sub(a, b));
    if (!std::isfinite(d)) return std::nullopt;
    return d;
}

std::optional<float> angle_deg(Vec3 from_dir, Vec3 to_dir) {
    auto a = normalize(from_dir);
    auto b = normalize(to_dir);
    if (!a || !b) return std::nullopt;
    float c = std::clamp(dot(*a, *b), -1.0f, 1.0f);
    return std::acos(c) * 180.0f / 3.14159265358979323846f;
}

}  // namespace robrun
