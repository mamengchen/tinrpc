#include "game/world_service.h"
#include "game.pb.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace game {

void WorldService::Enter(const std::string& player_id, const std::string& name, int64_t now_ms) {
    Player player;
    player.player_id = player_id;
    player.name = name;
    player.last_move_ms = now_ms;
    player.last_broadcast_ms = now_ms - kMinBroadcastIntervalMs;
    players_[player_id] = std::move(player);

    SendWorldStateTo(player_id);
    BroadcastEnter(players_.at(player_id));
}

void WorldService::Leave(const std::string& player_id) {
    if (players_.erase(player_id) != 0) {
        BroadcastLeave(player_id);
    }
}

MoveApplyResult WorldService::TryMove(const std::string& player_id, float x, float y, float z,
                                      float yaw, int64_t now_ms) {
    auto it = players_.find(player_id);
    if (it == players_.end()) {
        return {false, "玩家不在世界中"};
    }

    Player& player = it->second;
    const float clamped_x = std::clamp(x, -kBound, kBound);
    const float clamped_y = std::clamp(y, -kBound, kBound);
    const float clamped_z = std::clamp(z, -kBound, kBound);
    const bool clamped = clamped_x != x || clamped_y != y || clamped_z != z;
    const float dx = clamped_x - player.x;
    const float dy = clamped_y - player.y;
    const float dz = clamped_z - player.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    const int64_t dt_ms = std::max(now_ms - player.last_move_ms, int64_t{1});
    const float max_distance = kMaxSpeed * static_cast<float>(dt_ms) / 1000.0f;

    if (clamped || distance > max_distance) {
        return {false, clamped ? "位置超出世界边界" : "移动速度过快",
                player.x, player.y, player.z};
    }

    player.x = clamped_x;
    player.y = clamped_y;
    player.z = clamped_z;
    player.yaw = yaw;
    player.last_move_ms = now_ms;
    BroadcastPose(player, now_ms, false);
    return {true, "", player.x, player.y, player.z};
}

bool WorldService::HasPlayer(const std::string& player_id) const {
    return players_.find(player_id) != players_.end();
}

void WorldService::SendWorldStateTo(const std::string& to) {
    WorldStateNtf state;
    for (const auto& [_, player] : players_) {
        auto* transform = state.add_players();
        transform->set_player_id(player.player_id);
        transform->set_player_name(player.name);
        transform->mutable_position()->set_x(player.x);
        transform->mutable_position()->set_y(player.y);
        transform->mutable_position()->set_z(player.z);
        transform->set_yaw(player.yaw);
    }

    std::string buffer;
    state.SerializeToString(&buffer);
    send_(to, "WorldStateNtf", {buffer.begin(), buffer.end()});
}

void WorldService::BroadcastEnter(const Player& player) {
    PlayerTransform transform;
    transform.set_player_id(player.player_id);
    transform.set_player_name(player.name);
    transform.mutable_position()->set_x(player.x);
    transform.mutable_position()->set_y(player.y);
    transform.mutable_position()->set_z(player.z);
    transform.set_yaw(player.yaw);

    std::string buffer;
    transform.SerializeToString(&buffer);
    const std::vector<uint8_t> body(buffer.begin(), buffer.end());
    for (const auto& [player_id, _] : players_) {
        if (player_id != player.player_id) {
            send_(player_id, "PlayerEnterNtf", body);
        }
    }
}

void WorldService::BroadcastLeave(const std::string& player_id) {
    WorldPlayerLeaveNtf leave;
    leave.set_player_id(player_id);

    std::string buffer;
    leave.SerializeToString(&buffer);
    const std::vector<uint8_t> body(buffer.begin(), buffer.end());
    for (const auto& [remaining_player_id, _] : players_) {
        send_(remaining_player_id, "PlayerLeaveNtf", body);
    }
}

void WorldService::BroadcastPose(const Player& player, int64_t now_ms, bool force) {
    auto it = players_.find(player.player_id);
    if (it == players_.end() ||
        (!force && now_ms - it->second.last_broadcast_ms < kMinBroadcastIntervalMs)) {
        return;
    }

    WorldStateNtf state;
    auto* transform = state.add_players();
    transform->set_player_id(player.player_id);
    transform->set_player_name(player.name);
    transform->mutable_position()->set_x(player.x);
    transform->mutable_position()->set_y(player.y);
    transform->mutable_position()->set_z(player.z);
    transform->set_yaw(player.yaw);

    std::string buffer;
    state.SerializeToString(&buffer);
    const std::vector<uint8_t> body(buffer.begin(), buffer.end());
    for (const auto& [player_id, _] : players_) {
        if (player_id != player.player_id) {
            send_(player_id, "WorldStateNtf", body);
        }
    }
    it->second.last_broadcast_ms = now_ms;
}

} // namespace game
