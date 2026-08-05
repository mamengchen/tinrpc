#pragma once
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

class WorldService {
public:
    using SendFn = std::function<void(const std::string& player_id, const std::string& method,
                                      const std::vector<uint8_t>& body)>;

    explicit WorldService(SendFn send) : send_(std::move(send)) {}

    void Enter(const std::string& player_id, const std::string& name, int64_t now_ms);
    void Leave(const std::string& player_id);
    MoveApplyResult TryMove(const std::string& player_id, float x, float y, float z, float yaw,
                            int64_t now_ms);
    bool HasPlayer(const std::string& player_id) const;

    static constexpr float kMaxSpeed = 10.0f;
    static constexpr float kBound = 500.0f;
    static constexpr int64_t kMinBroadcastIntervalMs = 50;

private:
    struct Player {
        std::string player_id;
        std::string name;
        float x = 0, y = 0, z = 0;
        float yaw = 0;
        int64_t last_move_ms = 0;
        int64_t last_broadcast_ms = 0;
    };

    SendFn send_;
    std::unordered_map<std::string, Player> players_;

    void SendWorldStateTo(const std::string& to);
    void BroadcastEnter(const Player& p);
    void BroadcastLeave(const std::string& player_id);
    void BroadcastPose(const Player& p, int64_t now_ms, bool force);
};

} // namespace game
