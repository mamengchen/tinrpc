#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace game {

struct SceneResource {
    std::string id;
    int type = 0;
    float x = 0, y = 0, z = 0;
    int remaining = 0;
};

struct SceneBuilding {
    std::string id;
    std::string owner_id;
    int type = 0;
    float x = 0, y = 0, z = 0;
    float yaw = 0;
};

struct SceneVoxelEdit {
    std::string room_id;
    int x = 0, y = 0, z = 0;
    int action = 0;
    int block_type = 0;
};

struct SceneSnapshot {
    std::string scene_id = "default";
    std::vector<SceneResource> resources;
    std::vector<SceneBuilding> buildings;
    std::vector<SceneVoxelEdit> voxel_edits;
    uint64_t next_building_id = 1;
    bool found = false;
};

/**
 * @brief MongoDB scenes 集合：默认世界资源/建筑存档
 * @note Load/Upsert 可在启动线程或 DbWorker 调用；同一 client 不跨线程并发。
 */
class SceneStore {
public:
    struct Result {
        bool ok = false;
        std::string error_msg;
    };

    SceneStore(std::string uri, std::string db_name);
    ~SceneStore();

    SceneStore(const SceneStore&) = delete;
    SceneStore& operator=(const SceneStore&) = delete;

    bool Init(std::string* err = nullptr);
    SceneSnapshot Load(const std::string& scene_id = "default");
    Result Upsert(const SceneSnapshot& snap);

private:
    std::string uri_;
    std::string db_name_;
    void* client_ = nullptr;
};

} // namespace game
