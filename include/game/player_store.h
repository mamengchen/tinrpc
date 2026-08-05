#pragma once

#include <string>

namespace game {

struct PlayerState {
    std::string player_id;
    float x = 0, y = 0, z = 0;
    float yaw = 0;
    int wood = 6;
    int stone = 3;
    bool found = false;
};

/**
 * @brief MongoDB players 集合：读档 / 写档
 * @note 仅在 DbWorker 线程调用
 */
class PlayerStore {
public:
    struct Result {
        bool ok = false;
        std::string error_msg;
    };

    PlayerStore(std::string uri, std::string db_name);
    ~PlayerStore();

    PlayerStore(const PlayerStore&) = delete;
    PlayerStore& operator=(const PlayerStore&) = delete;

    bool Init(std::string* err = nullptr);

    /// 无档时 found=false，仍 ok=true（使用默认状态）
    PlayerState Load(const std::string& player_id);
    Result Upsert(const PlayerState& state);

private:
    std::string uri_;
    std::string db_name_;
    void* client_ = nullptr;
};

} // namespace game
