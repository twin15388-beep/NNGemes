// Изолированные тесты чистой математики. Без игры, без памяти.
// Запуск: ./robrun_tests (exit 0 = все прошли).

#include <cmath>
#include <cstdio>

#include "robrun/config.h"
#include "robrun/esp.h"
#include "robrun/math.h"

namespace {

int failures = 0;

void check(bool cond, const char* name) {
    if (!cond) {
        std::printf("FAIL: %s\n", name);
        ++failures;
    } else {
        std::printf("ok: %s\n", name);
    }
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

}  // namespace

int main() {
    using namespace robrun;

    // Камера в начале координат, смотрит +Z, FOV 90, viewport 1920x1080.
    Camera cam;
    cam.pos = {0, 0, 0};
    cam.fwd = {0, 0, 1};
    cam.right = {1, 0, 0};
    cam.up = {0, 1, 0};
    cam.fov_deg = 90.0f;
    cam.valid = true;
    Viewport vp{1920, 1080};

    // Точка прямо перед камерой -> центр экрана.
    {
        auto p = world_to_screen(cam, {0, 0, 10}, vp);
        check(p.has_value(), "center projects");
        if (p) {
            check(near(p->point.x, 960.0f, 1.0f), "center x=960");
            check(near(p->point.y, 540.0f, 1.0f), "center y=540");
            check(p->on_screen, "center on_screen");
            check(near(p->point.depth, 10.0f, 1e-3f), "center depth=10");
        }
    }

    // Точка за камерой -> nullopt (не Bounds-ложняк, а честный отказ).
    check(!world_to_screen(cam, {0, 0, -5}, vp).has_value(), "behind camera rejected");

    // Точка сбоку за пределами FOV -> проекция есть, on_screen=false.
    {
        auto p = world_to_screen(cam, {100, 0, 10}, vp);
        check(p.has_value(), "off-screen still projects");
        if (p) check(!p->on_screen, "off-screen flagged");
    }

    // NaN/Inf -> nullopt.
    check(!world_to_screen(cam, {NAN, 0, 10}, vp).has_value(), "NaN rejected");
    check(!world_to_screen(cam, {0, 0, INFINITY}, vp).has_value(), "Inf rejected");

    // Невалидная камера / viewport / FOV -> nullopt.
    {
        Camera bad = cam;
        bad.valid = false;
        check(!world_to_screen(bad, {0, 0, 10}, vp).has_value(), "invalid camera rejected");
        check(!world_to_screen(cam, {0, 0, 10}, Viewport{0, 1080}).has_value(),
              "zero viewport rejected");
        Camera badfov = cam;
        badfov.fov_deg = 0.0f;
        check(!world_to_screen(badfov, {0, 0, 10}, vp).has_value(), "zero FOV rejected");
    }

    // Вырожденный right/up -> fallback на world-up, проекция всё равно работает.
    {
        Camera deg = cam;
        deg.right = {0, 0, 0};
        deg.up = {0, 0, 0};
        auto p = world_to_screen(deg, {0, 0, 10}, vp);
        check(p.has_value(), "degenerate basis falls back");
        if (p) check(near(p->point.x, 960.0f, 1.0f), "fallback center x");
    }

    // Дистанция и угол.
    check(distance({0, 0, 0}, {3, 4, 0}).has_value() &&
              near(*distance({0, 0, 0}, {3, 4, 0}), 5.0f),
          "distance 3-4-5");
    check(!distance({NAN, 0, 0}, {0, 0, 0}).has_value(), "distance NaN rejected");
    {
        auto a = angle_deg({0, 0, 1}, {0, 0, 1});
        check(a.has_value() && near(*a, 0.0f, 1e-2f), "angle 0");
        auto b = angle_deg({0, 0, 1}, {0, 0, -1});
        check(b.has_value() && near(*b, 180.0f, 1e-2f), "angle 180");
    }

    // ESP-фильтр: мёртвые/локальные/дальние отсекаются, сортировка по дистанции.
    {
        std::vector<Entity> ents = {
            Entity{.id = 1, .position = {0, 0, 10}, .distance = 10.0f},
            Entity{.id = 2, .position = {0, 0, 5}, .distance = 5.0f},
            Entity{.id = 3, .position = {0, 0, 1}, .is_dead = true, .distance = 1.0f},
            Entity{.id = 4, .position = {0, 0, 1}, .is_local = true, .distance = 1.0f},
            Entity{.id = 5, .position = {0, 0, 999}, .distance = 999.0f},
        };
        EspConfig cfg;
        cfg.max_distance = 400.0f;
        auto f = filter_entities(ents, cfg);
        check(f.size() == 2, "filter keeps 2");
        if (f.size() == 2) {
            check(f[0]->id == 2 && f[1]->id == 1, "filter sorted by distance");
        }
    }

    // Конфиг: VERIFIED/конфликты честно видны, UNKNOWN отличим от нуля.
    check(offset_status() != nullptr, "offset_status present");
    check(kWorldEntities == 0x38, "kWorldEntities=0x38");
    check(kWemBattleChars == 0x18, "kWemBattleChars=0x18");
    check(kCharTransform == 0x58, "kCharTransform=0x58");
    check(kCamFwd == 0x3C && kCamRight == 0x48 && kCamUp == 0x54, "camera basis");
    check(kSfWorldS2.has_value() && *kSfWorldS2 == 0x81B7148, "static conflict gated");

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TESTS FAILED\n", failures);
    return 1;
}
