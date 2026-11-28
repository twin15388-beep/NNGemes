#include "robrun/esp.h"

#include <algorithm>

namespace robrun {

bool is_eligible(const Entity& e, const EspConfig& cfg) {
    if (!cfg.enabled) return false;
    if (e.is_dead || e.is_local) return false;
    if (e.distance > cfg.max_distance) return false;
    if (e.distance < 0.0f) return false;
    return true;
}

std::vector<const Entity*> filter_entities(const std::vector<Entity>& entities,
                                           const EspConfig& cfg) {
    std::vector<const Entity*> out;
    out.reserve(entities.size());
    for (const auto& e : entities) {
        if (is_eligible(e, cfg)) out.push_back(&e);
    }
    std::sort(out.begin(), out.end(),
              [](const Entity* a, const Entity* b) { return a->distance < b->distance; });
    return out;
}

}  // namespace robrun
