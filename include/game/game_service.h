#pragma once

#include "game/room_manager.h"
#include "game/room_service.h"
#include "game/server_metrics.h"
#include "game/match_service.h"
#include "game/broadcast.h"
#include "game/world_service.h"
#include "game/account_store.h"
#include "game/db_worker.h"
#include "game/player_store.h"
#include "game/scene_store.h"
#include "game/timer_manager.h"
#include "game/frame_sync.h"
#include "rpc/event_loop.h"
#include "rpc/dispatch.h"
#include "rpc/connection.h"
#include "rpc/protocol.h"

#include <cstdint>
#include <unordered_map>
#include <string>
#include <functional>
#include <memory>
#include <vector>

namespace game {

/**
 * @brief GameService — 游戏服务端集中入口
 *
 * 持有并组装: RoomManager + MatchService + Broadcast + TimerManager
 *             + FrameSyncManager(per-room) + Dispatch(全部RPC)
 *             + AccountStore / DbWorker（Mongo 账号）
 *
 * @note 业务回调在 EventLoop IO 线程；Mongo/bcrypt 在 DbWorker 线程。
 */
class GameService {
public:
    using SendToPlayerFn =
        std::function<void(const std::string& player_id, const std::vector<uint8_t>& data)>;
    using OnDisconnectFn = std::function<void(int fd)>;

    GameService();
    ~GameService();

    GameService(const GameService&) = delete;
    GameService& operator=(const GameService&) = delete;

    RoomManager& room_mgr() { return room_mgr_; }
    MatchQueue& match_queue() { return match_queue_; }
    TimerManager& timer() { return timer_; }
    Broadcast& broadcast() { return *broadcast_; }
    rpc::Dispatch& dispatch() { return dispatch_; }

    void RegisterPlayerConn(const std::string& player_id, rpc::Connection* conn);
    void UnregisterPlayerConn(const std::string& player_id);
    rpc::Connection* GetPlayerConn(const std::string& player_id) const;

    void SetFrameCallback(FrameSyncManager* fsm, const std::string& room_id);

    void Run(uint16_t port);
    void Stop();

private:
    void OnServerFrame(const rpc::Frame& frame, rpc::Connection* conn);
    void HandleRegister(const rpc::Frame& frame, rpc::Connection* conn);
    void HandleLogin(const rpc::Frame& frame, rpc::Connection* conn);
    void HandleSelectMap(const rpc::Frame& frame, rpc::Connection* conn);
    void HandleVoxelEdit(const rpc::Frame& frame, rpc::Connection* conn);
    void HandleMove(const rpc::Frame& frame, rpc::Connection* conn);
    void HandleGather(const rpc::Frame& frame, rpc::Connection* conn);
    void HandlePlaceBuilding(const rpc::Frame& frame, rpc::Connection* conn);

    int64_t NowMs() const;
    void SendToPlayer(const std::string& player_id, const std::string& method,
                      const std::vector<uint8_t>& body);
    void OnPlayerDisconnected(int fd);
    void OnMatchFound(const std::string& p1, double s1, const std::string& p2, double s2);

    RoomManager room_mgr_;
    MatchQueue match_queue_;
    TimerManager timer_;
    std::unique_ptr<Broadcast> broadcast_;
    std::unique_ptr<WorldService> world_;
    std::unique_ptr<RoomServiceImpl> room_svc_;
    std::unique_ptr<AccountStore> accounts_;
    std::unique_ptr<PlayerStore> players_;
    std::unique_ptr<SceneStore> scenes_;
    std::unique_ptr<DbWorker> db_worker_;
    ServerMetrics metrics_;

    void SavePlayerAsync(const std::string& player_id);
    void SaveSceneAsync();
    void LoadOrSeedScene();

    rpc::EventLoop loop_;
    rpc::Dispatch dispatch_;

    std::unordered_map<std::string, rpc::Connection*> player_conns_;
    std::unordered_map<int, std::string> fd_to_player_;
    std::unordered_map<int, rpc::Connection*> active_conns_;
};

} // namespace game
