#pragma once

// Чистая математика ESP: без Win32, без драйвера, без памяти.
// Система координат: Unity left-handed, Y-up, +Z forward.
// FOV: вертикальный, градусы. Viewport: пиксели (уже с учётом DPI).
// World-to-screen обязан обрабатывать: точку за камерой, NaN/Inf,
// нулевой viewport, вырожденный базис камеры.

#include <cmath>
#include <optional>

namespace robrun {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Camera {
    Vec3 pos{};
    Vec3 fwd{0.0f, 0.0f, 1.0f};    // +Z forward (Unity)
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    float fov_deg = 90.0f;         // вертикальный FOV, градусы
    bool valid = false;
};

struct ScreenPoint {
    float x = 0.0f, y = 0.0f;
    float depth = 0.0f;            // расстояние вдоль forward (>0)
};

struct Viewport {
    int width = 0, height = 0;
};

// Нормализация с проверкой конечности. nullopt при нулевом/неконечном векторе.
std::optional<Vec3> normalize(Vec3 v);

// World -> screen. nullopt: камера невалидна, точка за камерой (depth<=0.05),
// неконечные входы, вырожденный FOV/viewport.
// on_screen: NDC в (-1.2, 1.2) — 20% запаса за краями, как в референсе
// (game_w2s). Возвращаемые координаты могут быть вне [0,w]x[0,h] при
// on_screen==false, если проекция конечна — вызывающий решает, рисовать ли.
struct Projection {
    ScreenPoint point{};
    bool on_screen = false;
};

std::optional<Projection> world_to_screen(const Camera& cam, Vec3 world, Viewport vp);

// Дистанция в метрах (Unity units). nullopt при неконечных входах.
std::optional<float> distance(Vec3 a, Vec3 b);

// Угол между направлением взгляда и направлением на цель, градусы [0,180].
// nullopt при нулевых/неконечных векторах.
std::optional<float> angle_deg(Vec3 from_dir, Vec3 to_dir);

}  // namespace robrun
