#pragma once

#include "game/scene_store.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace game {

struct MoveApplyResult {
    bool success = false;
    std::string error_msg;
    float corrected_x = 0, corrected_y = 0, corrected_z = 0;
};

struct GatherApplyResult {
    bool success = false;
    std::string error_msg;
    std::string resource_id;
    int resource_type = 0;
    float x = 0, y = 0, z = 0;
    int remaining = 0;
    int wood = 0;
    int stone = 0;
};

struct BuildingApplyResult {
    bool success = false;
    std::string error_msg;
    std::string building_id;
    std::string owner_id;
    int building_type = 0;
    float x = 0, y = 0, z = 0;
    float yaw = 0;
    int wood = 0;
    int stone = 0;
};

class WorldService {
public:
    using SendFn = std::function<void(const std::string& player_id, const std::string& method,
                                      const std::vector<uint8_t>& body)>;

    explicit WorldService(SendFn send);

    void SeedDefaultScene();
    void LoadScene(const SceneSnapshot& snap);
    SceneSnapshot ExportScene(const std::string& scene_id = "default") const;

    void Enter(const std::string& player_id, const std::string& name, int64_t now_ms);
    /// 使用存档状态进入世界（found 档或默认值由调用方填好）
    void EnterWithState(const std::string& player_id, const std::string& name, int64_t now_ms,
                        float x, float y, float z, float yaw, int wood, int stone);
    void Leave(const std::string& player_id);
    bool SelectRoom(const std::string& player_id, const std::string& room_id, int map_id,
                    int64_t now_ms);
    /// 取当前内存快照；玩家不在世界返回 false
    bool GetSnapshot(const std::string& player_id, float* x, float* y, float* z, float* yaw,
                     int* wood, int* stone) const;
    MoveApplyResult TryMove(const std::string& player_id, float x, float y, float z, float yaw,
                            int64_t now_ms, int appearance = 0);
    GatherApplyResult TryGather(const std::string& player_id, const std::string& resource_id);
    BuildingApplyResult TryPlaceBuilding(const std::string& player_id, int building_type, float x,
                                         float y, float z, float yaw);
    bool HasPlayer(const std::string& player_id) const;
    bool ApplyVoxelEdit(const std::string& player_id, int x, int y, int z, int action,
                        int block_type, std::string* error_msg);

    static constexpr float kMaxSpeed = 15.0f;
    static constexpr float kMoveBurstAllowance = 15.0f;
    static constexpr float kInitialMoveAllowance = 1.0f;
    static constexpr float kBound = 500.0f;
    static constexpr float kInteractDistance = 4.0f;
    static constexpr int64_t kMinBroadcastIntervalMs = 50;

private:
    struct Player {
        std::string player_id;
        std::string name;
        std::string room_id = "lobby";
        int map_id = 0;
        float x = 0, y = 0, z = 0;
        float yaw = 0;
        int appearance = 0;
        int wood = 6;
        int stone = 3;
        int64_t last_move_ms = 0;
        float move_allowance = kInitialMoveAllowance;
        int64_t last_broadcast_ms = 0;
    };

    struct Resource {
        std::string id;
        int type = 0;
        float x = 0, y = 0, z = 0;
        int remaining = 0;
    };

    struct BuildingState {
        std::string id;
        std::string owner_id;
        int type = 0;
        float x = 0, y = 0, z = 0;
        float yaw = 0;
    };
    struct VoxelEditState { int x = 0, y = 0, z = 0, action = 0, block_type = 0; };

    SendFn send_;
    std::unordered_map<std::string, Player> players_;
    std::unordered_map<std::string, Resource> resources_;
    std::vector<BuildingState> buildings_;
    uint64_t next_building_id_ = 1;
    std::unordered_map<std::string, std::unordered_map<std::string, VoxelEditState>> voxel_edits_;

    void SendWorldStateTo(const std::string& to);
    void BroadcastEnter(const Player& p);
    void BroadcastLeave(const std::string& player_id, const std::string& room_id);
    void BroadcastPose(const Player& p, int64_t now_ms, bool force);
    void BroadcastResource(const Resource& resource);
    void BroadcastBuilding(const BuildingState& building);
    void BroadcastVoxelEdit(const std::string& room_id, const VoxelEditState& edit);
};

} // namespace game
