#include "game/world_service.h"
#include "game.pb.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace game {
namespace {

float Distance(float ax, float ay, float az, float bx, float by, float bz) {
    const float dx = ax - bx;
    const float dy = ay - by;
    const float dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void FillVec3(Vec3* out, float x, float y, float z) {
    out->set_x(x);
    out->set_y(y);
    out->set_z(z);
}

} // namespace

WorldService::WorldService(SendFn send) : send_(std::move(send)) {
}

void WorldService::SeedDefaultScene() {
    resources_.clear();
    buildings_.clear();
    next_building_id_ = 1;
    const Resource seeded[] = {
        {"tree_1", RESOURCE_WOOD, -6, 0, -5, 6}, {"tree_2", RESOURCE_WOOD, 7, 0, -4, 6},
        {"tree_3", RESOURCE_WOOD, -9, 0, 7, 6},  {"tree_4", RESOURCE_WOOD, 10, 0, 8, 6},
        {"rock_1", RESOURCE_STONE, -4, 0, 8, 5}, {"rock_2", RESOURCE_STONE, 5, 0, 6, 5},
        {"rock_3", RESOURCE_STONE, 9, 0, -9, 5},
    };
    for (const auto& resource : seeded)
        resources_.emplace(resource.id, resource);
}

void WorldService::LoadScene(const SceneSnapshot& snap) {
    resources_.clear();
    buildings_.clear();
    for (const auto& r : snap.resources) {
        Resource resource;
        resource.id = r.id;
        resource.type = r.type;
        resource.x = r.x;
        resource.y = r.y;
        resource.z = r.z;
        resource.remaining = r.remaining;
        resources_.emplace(resource.id, resource);
    }
    for (const auto& b : snap.buildings) {
        BuildingState building;
        building.id = b.id;
        building.owner_id = b.owner_id;
        building.type = b.type;
        building.x = b.x;
        building.y = b.y;
        building.z = b.z;
        building.yaw = b.yaw;
        buildings_.push_back(building);
    }
    next_building_id_ = snap.next_building_id > 0 ? snap.next_building_id : 1;
}

SceneSnapshot WorldService::ExportScene(const std::string& scene_id) const {
    SceneSnapshot snap;
    snap.scene_id = scene_id;
    snap.next_building_id = next_building_id_;
    snap.found = true;
    for (const auto& [_, r] : resources_) {
        SceneResource out;
        out.id = r.id;
        out.type = r.type;
        out.x = r.x;
        out.y = r.y;
        out.z = r.z;
        out.remaining = r.remaining;
        snap.resources.push_back(out);
    }
    for (const auto& b : buildings_) {
        SceneBuilding out;
        out.id = b.id;
        out.owner_id = b.owner_id;
        out.type = b.type;
        out.x = b.x;
        out.y = b.y;
        out.z = b.z;
        out.yaw = b.yaw;
        snap.buildings.push_back(out);
    }
    return snap;
}

void WorldService::Enter(const std::string& player_id, const std::string& name, int64_t now_ms) {
    float x = static_cast<float>(players_.size() % 4) * 2.0f;
    float z = static_cast<float>(players_.size() / 4) * 2.0f;
    // 若已在线则保持坐标；新进入用网格出生点
    auto existing = players_.find(player_id);
    if (existing != players_.end()) {
        EnterWithState(player_id, name, now_ms, existing->second.x, existing->second.y,
                       existing->second.z, existing->second.yaw, existing->second.wood,
                       existing->second.stone);
        return;
    }
    EnterWithState(player_id, name, now_ms, x, 0.f, z, 0.f, 6, 3);
}

void WorldService::EnterWithState(const std::string& player_id, const std::string& name,
                                  int64_t now_ms, float x, float y, float z, float yaw, int wood,
                                  int stone) {
    auto [it, inserted] = players_.try_emplace(player_id);
    Player& player = it->second;
    player.player_id = player_id;
    player.name = name;
    player.x = x;
    player.y = y;
    player.z = z;
    player.yaw = yaw;
    player.wood = wood;
    player.stone = stone;
    player.last_move_ms = now_ms;
    player.move_allowance = kInitialMoveAllowance;
    player.last_broadcast_ms = now_ms - kMinBroadcastIntervalMs;
    (void)inserted;
    SendWorldStateTo(player_id);
    BroadcastEnter(player);
}

void WorldService::Leave(const std::string& player_id) {
    auto it = players_.find(player_id);
    if (it == players_.end()) return;
    const std::string room_id = it->second.room_id;
    players_.erase(it);
    BroadcastLeave(player_id, room_id);
}

bool WorldService::SelectRoom(const std::string& player_id, const std::string& room_id,
                              int map_id, int64_t now_ms) {
    auto it = players_.find(player_id);
    if (it == players_.end() || room_id.empty() || map_id < 0 || map_id > 2) return false;
    Player& player = it->second;
    BroadcastLeave(player_id, player.room_id);
    player.room_id = room_id;
    player.map_id = map_id;
    player.x = static_cast<float>(std::hash<std::string>{}(player_id) % 5) - 2.0f;
    player.y = 0.0f;
    player.z = 0.0f;
    player.last_move_ms = now_ms;
    player.move_allowance = kInitialMoveAllowance;
    SendWorldStateTo(player_id);
    BroadcastEnter(player);
    return true;
}

bool WorldService::GetSnapshot(const std::string& player_id, float* x, float* y, float* z,
                               float* yaw, int* wood, int* stone) const {
    auto it = players_.find(player_id);
    if (it == players_.end())
        return false;
    const Player& p = it->second;
    if (x)
        *x = p.x;
    if (y)
        *y = p.y;
    if (z)
        *z = p.z;
    if (yaw)
        *yaw = p.yaw;
    if (wood)
        *wood = p.wood;
    if (stone)
        *stone = p.stone;
    return true;
}

MoveApplyResult WorldService::TryMove(const std::string& player_id, float x, float y, float z,
                                      float yaw, int64_t now_ms, int appearance) {
    auto it = players_.find(player_id);
    if (it == players_.end()) return {false, "player is not in world"};
    Player& player = it->second;
    const float clamped_x = std::clamp(x, -kBound, kBound);
    const float clamped_y = std::clamp(y, -kBound, kBound);
    const float clamped_z = std::clamp(z, -kBound, kBound);
    const bool clamped = clamped_x != x || clamped_y != y || clamped_z != z;
    const float distance = Distance(clamped_x, clamped_y, clamped_z, player.x, player.y, player.z);
    const int64_t dt_ms = std::max(now_ms - player.last_move_ms, int64_t{0});
    player.move_allowance = std::min(
        kMoveBurstAllowance,
        player.move_allowance + kMaxSpeed * static_cast<float>(dt_ms) / 1000.0f);
    player.last_move_ms = now_ms;
    if (clamped || distance > player.move_allowance) {
        return {false, clamped ? "position outside world" : "movement too fast", player.x,
                player.y, player.z};
    }

    player.move_allowance = std::max(0.0f, player.move_allowance - distance);
    player.x = clamped_x;
    player.y = clamped_y;
    player.z = clamped_z;
    player.yaw = yaw;
    player.appearance = std::clamp(appearance, 0, 3);
    BroadcastPose(player, now_ms, false);
    return {true, "", player.x, player.y, player.z};
}

GatherApplyResult WorldService::TryGather(const std::string& player_id,
                                          const std::string& resource_id) {
    auto player_it = players_.find(player_id);
    auto resource_it = resources_.find(resource_id);
    if (player_it == players_.end()) return {false, "player is not in world"};
    if (resource_it == resources_.end()) return {false, "resource not found"};
    Player& player = player_it->second;
    Resource& resource = resource_it->second;
    GatherApplyResult result;
    result.resource_id = resource.id;
    result.resource_type = resource.type;
    result.x = resource.x;
    result.y = resource.y;
    result.z = resource.z;
    result.remaining = resource.remaining;
    result.wood = player.wood;
    result.stone = player.stone;
    if (resource.remaining <= 0) {
        result.error_msg = "resource depleted";
        return result;
    }
    if (Distance(player.x, player.y, player.z, resource.x, resource.y, resource.z) >
        kInteractDistance) {
        result.error_msg = "resource is too far away";
        return result;
    }
    --resource.remaining;
    if (resource.type == RESOURCE_WOOD) ++player.wood;
    if (resource.type == RESOURCE_STONE) ++player.stone;
    result.success = true;
    result.remaining = resource.remaining;
    result.wood = player.wood;
    result.stone = player.stone;
    BroadcastResource(resource);
    return result;
}

BuildingApplyResult WorldService::TryPlaceBuilding(const std::string& player_id,
                                                    int building_type, float x, float y, float z,
                                                    float yaw) {
    auto player_it = players_.find(player_id);
    if (player_it == players_.end()) return {false, "player is not in world"};
    Player& player = player_it->second;
    BuildingApplyResult result;
    result.owner_id = player_id;
    result.building_type = building_type;
    result.x = std::round(x);
    result.y = std::round(y);
    result.z = std::round(z);
    result.yaw = std::round(yaw / 90.0f) * 90.0f;
    result.wood = player.wood;
    result.stone = player.stone;
    int wood_cost = 0, stone_cost = 0;
    if (building_type == BUILDING_FLOOR) wood_cost = 2;
    else if (building_type == BUILDING_WALL) wood_cost = 3;
    else if (building_type == BUILDING_CAMPFIRE) stone_cost = 3;
    else {
        result.error_msg = "unknown building type";
        return result;
    }
    if (Distance(player.x, player.y, player.z, result.x, result.y, result.z) > 8.0f) {
        result.error_msg = "build location is too far away";
        return result;
    }
    if (player.wood < wood_cost || player.stone < stone_cost) {
        result.error_msg = "not enough resources";
        return result;
    }
    for (const auto& existing : buildings_) {
        if (Distance(existing.x, existing.y, existing.z, result.x, result.y, result.z) < 0.8f) {
            result.error_msg = "build location is occupied";
            return result;
        }
    }
    player.wood -= wood_cost;
    player.stone -= stone_cost;
    BuildingState building;
    building.id = "building_" + std::to_string(next_building_id_++);
    building.owner_id = player_id;
    building.type = building_type;
    building.x = result.x;
    building.y = result.y;
    building.z = result.z;
    building.yaw = result.yaw;
    buildings_.push_back(building);
    result.success = true;
    result.building_id = building.id;
    result.wood = player.wood;
    result.stone = player.stone;
    BroadcastBuilding(building);
    return result;
}

bool WorldService::HasPlayer(const std::string& player_id) const {
    return players_.find(player_id) != players_.end();
}

void WorldService::SendWorldStateTo(const std::string& to) {
    WorldStateNtf state;
    for (const auto& [_, player] : players_) {
        auto target = players_.find(to);
        if (target == players_.end() || player.room_id != target->second.room_id) continue;
        auto* transform = state.add_players();
        transform->set_player_id(player.player_id);
        transform->set_player_name(player.name);
        FillVec3(transform->mutable_position(), player.x, player.y, player.z);
        transform->set_yaw(player.yaw);
        transform->set_appearance(player.appearance);
    }
    for (const auto& [_, resource] : resources_) {
        auto* out = state.add_resources();
        out->set_resource_id(resource.id);
        out->set_resource_type(static_cast<ResourceType>(resource.type));
        FillVec3(out->mutable_position(), resource.x, resource.y, resource.z);
        out->set_remaining(resource.remaining);
    }
    for (const auto& building : buildings_) {
        auto* out = state.add_buildings();
        out->set_building_id(building.id);
        out->set_owner_id(building.owner_id);
        out->set_building_type(static_cast<BuildingType>(building.type));
        FillVec3(out->mutable_position(), building.x, building.y, building.z);
        out->set_yaw(building.yaw);
    }
    auto target = players_.find(to);
    if (target != players_.end()) {
        for (const auto& [_, edit] : voxel_edits_[target->second.room_id]) {
            auto* out = state.add_voxel_edits();
            out->set_x(edit.x); out->set_y(edit.y); out->set_z(edit.z);
            out->set_action(edit.action); out->set_block_type(edit.block_type);
        }
    }
    std::string buffer;
    state.SerializeToString(&buffer);
    send_(to, "WorldStateNtf", {buffer.begin(), buffer.end()});
}

bool WorldService::ApplyVoxelEdit(const std::string& player_id, int x, int y, int z, int action,
                                  int block_type, std::string* error_msg) {
    auto it = players_.find(player_id);
    if (it == players_.end()) { if (error_msg) *error_msg = "player is not in world"; return false; }
    const Player& player = it->second;
    if (std::hypot(static_cast<float>(x) - player.x, static_cast<float>(z) - player.z) > 5.0f) {
        if (error_msg) *error_msg = "block is too far away"; return false;
    }
    if ((action != 1 && action != 2) || block_type < 0 || block_type > 5 || y < -1 || y > 4) {
        if (error_msg) *error_msg = "invalid voxel edit"; return false;
    }
    VoxelEditState edit{x, y, z, action, block_type};
    const std::string key = std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z);
    voxel_edits_[player.room_id][key] = edit;
    BroadcastVoxelEdit(player.room_id, edit);
    return true;
}

void WorldService::BroadcastVoxelEdit(const std::string& room_id, const VoxelEditState& edit) {
    VoxelEditNtf ntf;
    auto* out = ntf.mutable_edit();
    out->set_x(edit.x); out->set_y(edit.y); out->set_z(edit.z);
    out->set_action(edit.action); out->set_block_type(edit.block_type);
    std::string buffer; ntf.SerializeToString(&buffer);
    const std::vector<uint8_t> body(buffer.begin(), buffer.end());
    for (const auto& [id, player] : players_)
        if (player.room_id == room_id) send_(id, "VoxelEditNtf", body);
}

void WorldService::BroadcastEnter(const Player& player) {
    PlayerTransform transform;
    transform.set_player_id(player.player_id);
    transform.set_player_name(player.name);
    FillVec3(transform.mutable_position(), player.x, player.y, player.z);
    transform.set_yaw(player.yaw);
    transform.set_appearance(player.appearance);
    std::string buffer;
    transform.SerializeToString(&buffer);
    const std::vector<uint8_t> body(buffer.begin(), buffer.end());
    for (const auto& [id, other] : players_)
        if (id != player.player_id && other.room_id == player.room_id) send_(id, "PlayerEnterNtf", body);
}

void WorldService::BroadcastLeave(const std::string& player_id, const std::string& room_id) {
    WorldPlayerLeaveNtf leave;
    leave.set_player_id(player_id);
    std::string buffer;
    leave.SerializeToString(&buffer);
    const std::vector<uint8_t> body(buffer.begin(), buffer.end());
    for (const auto& [id, other] : players_)
        if (other.room_id == room_id) send_(id, "PlayerLeaveNtf", body);
}

void WorldService::BroadcastPose(const Player& player, int64_t now_ms, bool force) {
    auto it = players_.find(player.player_id);
    if (it == players_.end() || (!force && now_ms - it->second.last_broadcast_ms < kMinBroadcastIntervalMs)) return;
    WorldStateNtf state;
    auto* transform = state.add_players();
    transform->set_player_id(player.player_id);
    transform->set_player_name(player.name);
    FillVec3(transform->mutable_position(), player.x, player.y, player.z);
    transform->set_yaw(player.yaw);
    transform->set_appearance(player.appearance);
    std::string buffer;
    state.SerializeToString(&buffer);
    const std::vector<uint8_t> body(buffer.begin(), buffer.end());
    for (const auto& [id, other] : players_)
        if (id != player.player_id && other.room_id == player.room_id) send_(id, "WorldStateNtf", body);
    it->second.last_broadcast_ms = now_ms;
}

void WorldService::BroadcastResource(const Resource& resource) {
    ResourceChangedNtf ntf;
    auto* out = ntf.mutable_resource();
    out->set_resource_id(resource.id);
    out->set_resource_type(static_cast<ResourceType>(resource.type));
    FillVec3(out->mutable_position(), resource.x, resource.y, resource.z);
    out->set_remaining(resource.remaining);
    std::string buffer;
    ntf.SerializeToString(&buffer);
    const std::vector<uint8_t> body(buffer.begin(), buffer.end());
    for (const auto& [id, _] : players_) send_(id, "ResourceChangedNtf", body);
}

void WorldService::BroadcastBuilding(const BuildingState& building) {
    BuildingPlacedNtf ntf;
    auto* out = ntf.mutable_building();
    out->set_building_id(building.id);
    out->set_owner_id(building.owner_id);
    out->set_building_type(static_cast<BuildingType>(building.type));
    FillVec3(out->mutable_position(), building.x, building.y, building.z);
    out->set_yaw(building.yaw);
    std::string buffer;
    ntf.SerializeToString(&buffer);
    const std::vector<uint8_t> body(buffer.begin(), buffer.end());
    for (const auto& [id, _] : players_) send_(id, "BuildingPlacedNtf", body);
}

} // namespace game
