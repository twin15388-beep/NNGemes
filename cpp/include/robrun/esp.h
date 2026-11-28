#pragma once

// ESP-сущности и фильтрация. Без памяти и рендера: только данные,
// которые внешний процесс уже прочитал (через драйвер), и решения,
// что из этого рисовать.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "robrun/math.h"

namespace robrun {

enum class Role : std::uint8_t { Cop = 0, Robber = 1, Citizen = 2, Unknown = 3 };

struct Entity {
    std::uint32_t id = 0;
    Vec3 position{};
    bool is_dead = false;
    bool is_local = false;
    bool is_knocked = false;
    bool is_in_vehicle = false;
    Role role = Role::Unknown;
    std::string name;
    std::string weapon;
    int health = 0;
    int max_health = 0;
    bool has_armor = false;
    float distance = 0.0f;  // заполняется при снапшоте
};

struct EspConfig {
    bool enabled = true;
    bool show_names = true;
    bool show_skeleton = true;
    bool show_distance = true;
    bool show_weapon = true;
    float max_distance = 400.0f;
    bool visible_only = false;  // требует LOS (не реализован — см. docs)
};

// Кости скелета (индексы в риге shared-characters_assets_all.bundle, 94 кости).
// Имена из offsets/robrun_rig.json (тот же build 26615022, UNVERIFIED).
inline constexpr int kBoneSpine1 = 0;   // M_spine_1_jnt (корень)
inline constexpr int kBoneSpine2 = 1;   // M_spine_2_jnt
inline constexpr int kBoneSpine3 = 2;   // M_spine_3_jnt
inline constexpr int kBoneNeck = 3;     // M_neck_jnt
inline constexpr int kBoneHead = 4;     // M_head_1_jnt
inline constexpr int kBoneLArm0 = 39;   // L_arm_0_jnt
inline constexpr int kBoneLArm2 = 41;   // L_arm_2_jnt
inline constexpr int kBoneRArm0 = 59;   // R_arm_0_jnt
inline constexpr int kBoneRArm2 = 61;   // R_arm_2_jnt
inline constexpr int kBoneLLeg1 = 84;   // L_leg_1_jnt
inline constexpr int kBoneLLeg3 = 86;   // L_leg_3_jnt
inline constexpr int kBoneRLeg1 = 89;   // R_leg_1_jnt
inline constexpr int kBoneRLeg3 = 91;   // R_leg_3_jnt

// Допустимость сущности для ESP (без учёта проекции).
bool is_eligible(const Entity& e, const EspConfig& cfg);

// Фильтр + сортировка по дистанции. Возвращает указатели на входные сущности
// (владение остаётся у вызывающего). Отбрасывает мёртвых/локальных/дальних.
std::vector<const Entity*> filter_entities(const std::vector<Entity>& entities,
                                           const EspConfig& cfg);

}  // namespace robrun
