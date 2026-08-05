#include "game/world_service.h"

namespace game {

void WorldService::Enter(const std::string&, const std::string&, int64_t) {}

void WorldService::Leave(const std::string&) {}

MoveApplyResult WorldService::TryMove(const std::string&, float, float, float, float, int64_t) {
    return {};
}

bool WorldService::HasPlayer(const std::string&) const { return false; }

void WorldService::SendWorldStateTo(const std::string&) {}

void WorldService::BroadcastEnter(const Player&) {}

void WorldService::BroadcastLeave(const std::string&) {}

void WorldService::BroadcastPose(const Player&, int64_t, bool) {}

} // namespace game
