#include "game/game_service.h"
#include "game/room_service.h"
#include "game/player_store.h"
#include "rpc/acceptor.h"
#include "game.pb.h"

#include <sys/epoll.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace game {
namespace {

const char* EnvOr(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? v : fallback;
}

} // namespace

GameService::GameService() {
    auto send_fn = [this](const std::string& player_id, const std::vector<uint8_t>& data) {
        auto it = player_conns_.find(player_id);
        if (it != player_conns_.end() && it->second) {
            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, "RoomEvent", data);
            it->second->Send(frame);
        }
    };
    broadcast_ = std::make_unique<Broadcast>(&room_mgr_, send_fn);
    world_ = std::make_unique<WorldService>(
        [this](const std::string& player_id, const std::string& method,
               const std::vector<uint8_t>& body) { SendToPlayer(player_id, method, body); });

    room_svc_ = std::make_unique<RoomServiceImpl>(&room_mgr_, broadcast_.get());
    RegisterRoomService(&dispatch_, room_svc_.get());

    match_queue_.SetMatchCallback([this](const std::string& p1, double s1, const std::string& p2,
                                         double s2) { OnMatchFound(p1, s1, p2, s2); });

    accounts_ = std::make_unique<AccountStore>(EnvOr("TINRPC_MONGO_URI", "mongodb://127.0.0.1:27017"),
                                               EnvOr("TINRPC_MONGO_DB", "tinrpc"));
    players_ = std::make_unique<PlayerStore>(EnvOr("TINRPC_MONGO_URI", "mongodb://127.0.0.1:27017"),
                                             EnvOr("TINRPC_MONGO_DB", "tinrpc"));
    scenes_ = std::make_unique<SceneStore>(EnvOr("TINRPC_MONGO_URI", "mongodb://127.0.0.1:27017"),
                                           EnvOr("TINRPC_MONGO_DB", "tinrpc"));
    std::string mongo_err;
    if (!accounts_->Init(&mongo_err)) {
        printf("[GameService] WARN: Mongo accounts init failed: %s\n", mongo_err.c_str());
    }
    mongo_err.clear();
    if (!players_->Init(&mongo_err)) {
        printf("[GameService] WARN: Mongo players init failed: %s\n", mongo_err.c_str());
    }
    mongo_err.clear();
    if (!scenes_->Init(&mongo_err)) {
        printf("[GameService] WARN: Mongo scenes init failed: %s\n", mongo_err.c_str());
    }
    LoadOrSeedScene();
    db_worker_ = std::make_unique<DbWorker>(&loop_);
    db_worker_->Start();

    dispatch_.RegisterMethod(
        "GetMetrics",
        [this](const std::vector<uint8_t>& /*body*/) -> std::optional<std::vector<uint8_t>> {
            GetMetricsRes res;
            res.set_success(true);
            auto snap = metrics_.GetSnapshot(static_cast<int32_t>(room_mgr_.GetAllRoomIds().size()),
                                             static_cast<int32_t>(match_queue_.QueueSize()));
            res.set_uptime_sec(snap.uptime_sec);
            res.set_active_connections(snap.active_connections);
            res.set_total_rooms(snap.total_rooms);
            res.set_total_requests(snap.total_requests);
            res.set_current_qps(snap.current_qps);
            res.set_avg_latency_us(snap.avg_latency_us);
            res.set_p50_latency_us(snap.p50_latency_us);
            res.set_p99_latency_us(snap.p99_latency_us);
            res.set_match_queue_size(snap.match_queue_size);
            res.set_error_count(snap.error_count);
            std::string buf;
            res.SerializeToString(&buf);
            return std::vector<uint8_t>(buf.begin(), buf.end());
        });

    dispatch_.RegisterMethod(
        "EnterMatch", [this](const std::vector<uint8_t>& body) -> std::optional<std::vector<uint8_t>> {
            MatchPlayerReq req;
            if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
                return std::nullopt;
            }
            MatchPlayerRes res;
            match_queue_.EnterQueue(req.player_id(), req.elo_score());
            match_queue_.TryMatch();
            res.set_success(true);
            std::string buf;
            res.SerializeToString(&buf);
            return std::vector<uint8_t>(buf.begin(), buf.end());
        });

    dispatch_.RegisterMethod(
        "CancelMatch",
        [this](const std::vector<uint8_t>& body) -> std::optional<std::vector<uint8_t>> {
            CancelMatchReq req;
            if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
                return std::nullopt;
            }
            CancelMatchRes res;
            match_queue_.CancelMatch(req.player_id());
            res.set_success(true);
            std::string buf;
            res.SerializeToString(&buf);
            return std::vector<uint8_t>(buf.begin(), buf.end());
        });

    printf("[GameService] 初始化完成: RoomService + World + AccountStore/DbWorker + Match\n");
}

GameService::~GameService() {
    if (db_worker_)
        db_worker_->Stop();
}

// ---- 玩家连接管理 ----

void GameService::RegisterPlayerConn(const std::string& player_id, rpc::Connection* conn) {
    player_conns_[player_id] = conn;
    fd_to_player_[conn->GetFd()] = player_id;
}

void GameService::UnregisterPlayerConn(const std::string& player_id) {
    auto it = player_conns_.find(player_id);
    if (it != player_conns_.end() && it->second) {
        fd_to_player_.erase(it->second->GetFd());
    }
    player_conns_.erase(player_id);
}

rpc::Connection* GameService::GetPlayerConn(const std::string& player_id) const {
    auto it = player_conns_.find(player_id);
    return it != player_conns_.end() ? it->second : nullptr;
}

int64_t GameService::NowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void GameService::SendToPlayer(const std::string& player_id, const std::string& method,
                               const std::vector<uint8_t>& body) {
    auto it = player_conns_.find(player_id);
    if (it == player_conns_.end() || !it->second) {
        return;
    }
    auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, method, body);
    it->second->Send(frame);
}

void GameService::HandleMove(const rpc::Frame& frame, rpc::Connection* conn) {
    auto player_it = fd_to_player_.find(conn->GetFd());
    if (player_it == fd_to_player_.end()) {
        MoveRes res;
        res.set_success(false);
        res.set_error_msg("玩家未登录");
        std::string buf;
        res.SerializeToString(&buf);
        auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response, "Move",
                                              std::vector<uint8_t>(buf.begin(), buf.end()));
        conn->Send(rsp);
        return;
    }

    MoveReq req;
    if (!req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()))) {
        metrics_.OnError();
        auto err =
            rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error, "Move", {});
        conn->Send(err);
        return;
    }

    const auto result = world_->TryMove(player_it->second, req.position().x(), req.position().y(),
                                        req.position().z(), req.yaw(), NowMs(), req.appearance());
    MoveRes res;
    res.set_success(result.success);
    res.set_error_msg(result.error_msg);
    res.mutable_corrected_position()->set_x(result.corrected_x);
    res.mutable_corrected_position()->set_y(result.corrected_y);
    res.mutable_corrected_position()->set_z(result.corrected_z);
    std::string buf;
    res.SerializeToString(&buf);
    auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response, "Move",
                                          std::vector<uint8_t>(buf.begin(), buf.end()));
    conn->Send(rsp);
}

void GameService::HandleSelectMap(const rpc::Frame& frame, rpc::Connection* conn) {
    SelectMapRes res;
    SelectMapReq req;
    auto player = fd_to_player_.find(conn->GetFd());
    if (player == fd_to_player_.end() || !req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()))) {
        res.set_success(false);
        res.set_error_msg("player is not logged in");
    } else {
        res.set_success(world_->SelectRoom(player->second, req.room_id(), req.map_id(), NowMs()));
        res.set_map_id(req.map_id());
        if (!res.success()) res.set_error_msg("invalid room or map");
    }
    std::string buf;
    res.SerializeToString(&buf);
    conn->Send(rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                          "SelectMap", {buf.begin(), buf.end()}));
}

void GameService::HandleVoxelEdit(const rpc::Frame& frame, rpc::Connection* conn) {
    VoxelEditReq req; VoxelEditRes res; std::string error;
    auto player = fd_to_player_.find(conn->GetFd());
    const bool parsed = req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()));
    const bool ok = player != fd_to_player_.end() && parsed &&
        world_->ApplyVoxelEdit(player->second, req.x(), req.y(), req.z(), req.action(), req.block_type(), &error);
    if (ok) SaveSceneAsync();
    res.set_success(ok); res.set_error_msg(error);
    if (parsed) { auto* e=res.mutable_edit(); e->set_x(req.x()); e->set_y(req.y()); e->set_z(req.z()); e->set_action(req.action()); e->set_block_type(req.block_type()); }
    std::string buf; res.SerializeToString(&buf);
    conn->Send(rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response, "VoxelEdit", {buf.begin(), buf.end()}));
}

void GameService::HandleGather(const rpc::Frame& frame, rpc::Connection* conn) {
    auto player_it = fd_to_player_.find(conn->GetFd());
    GatherReq req;
    if (player_it == fd_to_player_.end() ||
        !req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()))) {
        auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                              "Gather", {});
        conn->Send(err);
        return;
    }
    const auto result = world_->TryGather(player_it->second, req.resource_id());
    if (result.success) {
        SavePlayerAsync(player_it->second);
        SaveSceneAsync();
    }
    GatherRes res;
    res.set_success(result.success);
    res.set_error_msg(result.error_msg);
    auto* resource = res.mutable_resource();
    resource->set_resource_id(result.resource_id);
    resource->set_resource_type(static_cast<ResourceType>(result.resource_type));
    resource->mutable_position()->set_x(result.x);
    resource->mutable_position()->set_y(result.y);
    resource->mutable_position()->set_z(result.z);
    resource->set_remaining(result.remaining);
    res.mutable_inventory()->set_wood(result.wood);
    res.mutable_inventory()->set_stone(result.stone);
    std::string buf;
    res.SerializeToString(&buf);
    conn->Send(rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response, "Gather",
                                          {buf.begin(), buf.end()}));
}

void GameService::HandlePlaceBuilding(const rpc::Frame& frame, rpc::Connection* conn) {
    auto player_it = fd_to_player_.find(conn->GetFd());
    PlaceBuildingReq req;
    if (player_it == fd_to_player_.end() ||
        !req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()))) {
        auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                              "PlaceBuilding", {});
        conn->Send(err);
        return;
    }
    const auto result = world_->TryPlaceBuilding(
        player_it->second, static_cast<int>(req.building_type()), req.position().x(),
        req.position().y(), req.position().z(), req.yaw());
    if (result.success) {
        SavePlayerAsync(player_it->second);
        SaveSceneAsync();
    }
    PlaceBuildingRes res;
    res.set_success(result.success);
    res.set_error_msg(result.error_msg);
    auto* building = res.mutable_building();
    building->set_building_id(result.building_id);
    building->set_owner_id(result.owner_id);
    building->set_building_type(static_cast<BuildingType>(result.building_type));
    building->mutable_position()->set_x(result.x);
    building->mutable_position()->set_y(result.y);
    building->mutable_position()->set_z(result.z);
    building->set_yaw(result.yaw);
    res.mutable_inventory()->set_wood(result.wood);
    res.mutable_inventory()->set_stone(result.stone);
    std::string buf;
    res.SerializeToString(&buf);
    conn->Send(rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                          "PlaceBuilding", {buf.begin(), buf.end()}));
}

// ---- 服务端帧回调 ----

void GameService::HandleRegister(const rpc::Frame& frame, rpc::Connection* conn) {
    RegisterReq req;
    if (!req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()))) {
        metrics_.OnError();
        conn->Send(rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error, "Register",
                                              {}));
        return;
    }

    const uint32_t rid = frame.request_id;
    const int fd = conn->GetFd();
    active_conns_[fd] = conn;
    const std::string username = req.username();
    const std::string password = req.password();

    db_worker_->Post([this, rid, fd, username, password]() {
        AccountStore::Result result = accounts_->CreateAccount(username, password);
        db_worker_->PostToLoop([this, rid, fd, result]() {
            auto it = active_conns_.find(fd);
            if (it == active_conns_.end() || !it->second)
                return;
            rpc::Connection* c = it->second;
            RegisterRes res;
            res.set_success(result.ok);
            res.set_error_msg(result.error_msg);
            if (result.ok)
                res.set_player_id(result.player_id);
            std::string buf;
            res.SerializeToString(&buf);
            c->Send(rpc::ProtocolFrame::Encode(rid, rpc::MessageType::Response, "Register",
                                               {buf.begin(), buf.end()}));
        });
    });
}

void GameService::HandleLogin(const rpc::Frame& frame, rpc::Connection* conn) {
    LoginReq req;
    if (!req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()))) {
        metrics_.OnError();
        conn->Send(
            rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error, "Login", {}));
        return;
    }

    const uint32_t rid = frame.request_id;
    const int fd = conn->GetFd();
    active_conns_[fd] = conn;
    const std::string username = req.username();
    const std::string password = req.password();

    db_worker_->Post([this, rid, fd, username, password]() {
        AccountStore::Result result = accounts_->VerifyCredentials(username, password);
        PlayerState saved;
        if (result.ok) {
            saved = players_->Load(result.player_id);
            if (!saved.found) {
                saved.player_id = result.player_id;
                saved.x = 0;
                saved.y = 0;
                saved.z = 0;
                saved.yaw = 0;
                saved.wood = 6;
                saved.stone = 3;
            }
        }
        db_worker_->PostToLoop([this, rid, fd, result, saved]() {
            auto it = active_conns_.find(fd);
            if (it == active_conns_.end() || !it->second)
                return;
            rpc::Connection* c = it->second;

            LoginRes res;
            if (!result.ok) {
                res.set_success(false);
                res.set_error_msg(result.error_msg.empty() ? "invalid credentials" : result.error_msg);
                std::string buf;
                res.SerializeToString(&buf);
                c->Send(rpc::ProtocolFrame::Encode(rid, rpc::MessageType::Response, "Login",
                                                   {buf.begin(), buf.end()}));
                metrics_.OnError();
                return;
            }

            auto old = fd_to_player_.find(fd);
            if (old != fd_to_player_.end() && old->second != result.player_id) {
                SavePlayerAsync(old->second);
                world_->Leave(old->second);
                UnregisterPlayerConn(old->second);
            }

            RegisterPlayerConn(result.player_id, c);
            metrics_.OnConnect();
            world_->EnterWithState(result.player_id, result.player_id, NowMs(), saved.x, saved.y,
                                   saved.z, saved.yaw, saved.wood, saved.stone);

            res.set_success(true);
            res.mutable_player_info()->set_player_id(result.player_id);
            res.mutable_player_info()->set_player_name(result.player_id);
            std::string buf;
            res.SerializeToString(&buf);
            c->Send(rpc::ProtocolFrame::Encode(rid, rpc::MessageType::Response, "Login",
                                               {buf.begin(), buf.end()}));
            printf("[GameService] 玩家登录: %s (pos=%.1f,%.1f,%.1f wood=%d stone=%d)\n",
                   result.player_id.c_str(), saved.x, saved.y, saved.z, saved.wood, saved.stone);
        });
    });
}

void GameService::OnServerFrame(const rpc::Frame& frame, rpc::Connection* conn) {
    auto t0 = std::chrono::steady_clock::now();
    metrics_.Tick();
    active_conns_[conn->GetFd()] = conn;

    if (frame.method_name == "Register") {
        HandleRegister(frame, conn);
        return;
    }

    if (frame.method_name == "Login") {
        HandleLogin(frame, conn);
        return;
    }

    if (frame.method_name == "SelectMap") {
        HandleSelectMap(frame, conn);
        return;
    }

    if (frame.method_name == "Move") {
        HandleMove(frame, conn);
        return;
    }
    if (frame.method_name == "VoxelEdit") { HandleVoxelEdit(frame, conn); return; }

    if (frame.method_name == "Gather") {
        HandleGather(frame, conn);
        return;
    }

    if (frame.method_name == "PlaceBuilding") {
        HandlePlaceBuilding(frame, conn);
        return;
    }

    // 其他方法 → Dispatch 分发
    auto rsp_body = dispatch_.Call(frame.method_name, frame.body);
    if (rsp_body) {
        auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                              frame.method_name, *rsp_body);
        conn->Send(rsp);
    } else {
        metrics_.OnError();
        auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                              frame.method_name, {});
        conn->Send(err);
    }

    auto t1 = std::chrono::steady_clock::now();
    double latency_us = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    metrics_.OnRequest(latency_us);
}

// ---- 断连回调 ----

void GameService::SavePlayerAsync(const std::string& player_id) {
    PlayerState st;
    st.player_id = player_id;
    if (!world_->GetSnapshot(player_id, &st.x, &st.y, &st.z, &st.yaw, &st.wood, &st.stone))
        return;
    db_worker_->Post([this, st]() {
        auto r = players_->Upsert(st);
        if (!r.ok) {
            printf("[GameService] WARN: save player %s failed: %s\n", st.player_id.c_str(),
                   r.error_msg.c_str());
        }
    });
}

void GameService::SaveSceneAsync() {
    SceneSnapshot snap = world_->ExportScene("default");
    db_worker_->Post([this, snap]() {
        auto r = scenes_->Upsert(snap);
        if (!r.ok) {
            printf("[GameService] WARN: save scene failed: %s\n", r.error_msg.c_str());
        }
    });
}

void GameService::LoadOrSeedScene() {
    auto snap = scenes_->Load("default");
    if (snap.found && (!snap.resources.empty() || !snap.buildings.empty())) {
        world_->LoadScene(snap);
        printf("[GameService] 已加载场景存档: resources=%zu buildings=%zu\n", snap.resources.size(),
               snap.buildings.size());
    } else {
        world_->SeedDefaultScene();
        printf("[GameService] 使用默认场景种子\n");
        // 首次写入，便于下次启动恢复
        auto seeded = world_->ExportScene("default");
        auto r = scenes_->Upsert(seeded);
        if (!r.ok) {
            printf("[GameService] WARN: seed scene upsert failed: %s\n", r.error_msg.c_str());
        }
    }
}

void GameService::OnPlayerDisconnected(int fd) {
    active_conns_.erase(fd);

    auto it = fd_to_player_.find(fd);
    if (it == fd_to_player_.end())
        return;

    std::string player_id = it->second;
    printf("[GameService] 玩家断连: %s (fd=%d)\n", player_id.c_str(), fd);

    SavePlayerAsync(player_id);

    std::string room_id = room_mgr_.GetPlayerRoom(player_id);
    if (!room_id.empty()) {
        room_mgr_.LeaveRoomAndNotify(room_id, player_id, broadcast_.get());
    }

    match_queue_.CancelMatch(player_id);
    world_->Leave(player_id);
    UnregisterPlayerConn(player_id);
    metrics_.OnDisconnect();
}

// ---- 匹配成功回调 ----

void GameService::OnMatchFound(const std::string& p1, double score1, const std::string& p2,
                               double score2) {
    printf("[GameService] 匹配成功: %s(%.0f) vs %s(%.0f)\n", p1.c_str(), score1, p2.c_str(), score2);

    // 1. 创建房间（p1 为房主）
    GameRoom::Config cfg;
    cfg.max_players = 2;
    auto result = room_mgr_.CreateRoom(p1, cfg);
    if (!result.ok)
        return;
    std::string rid = result.room_id;

    auto* room = room_mgr_.GetRoom(rid);
    room->SetState(ROOM_STATE_WAITING);
    room_mgr_.JoinRoom(rid, p2);

    // 2. 通知双方
    MatchFoundNtf ntf;
    ntf.set_room_id(rid);
    ntf.set_timeout_sec(30);

    std::string buf;
    ntf.set_player_id(p1);
    ntf.set_opponent_id(p2);
    ntf.SerializeToString(&buf);
    broadcast_->BroadcastToRoomExcept(rid, p2, std::vector<uint8_t>(buf.begin(), buf.end()));

    ntf.set_player_id(p2);
    ntf.set_opponent_id(p1);
    ntf.SerializeToString(&buf);
    broadcast_->BroadcastToRoomExcept(rid, p1, std::vector<uint8_t>(buf.begin(), buf.end()));

    // 3. 设置超时定时器
    timer_.Schedule(30000, [this, rid, p1, p2, score1, score2]() {
        auto* r = room_mgr_.GetRoom(rid);
        if (r && r->state() != ROOM_STATE_PLAYING) {
            room_mgr_.RemoveRoom(rid);
            match_queue_.EnterQueue(p1, score1);
            match_queue_.EnterQueue(p2, score2);
            printf("[GameService] 匹配超时: room=%s, 双方重新入队\n", rid.c_str());
        }
    });

    printf("[GameService] 房间=%s, 双方=%s/%s\n", rid.c_str(), p1.c_str(), p2.c_str());
}

// ---- 启动/停止 ----

void GameService::Run(uint16_t port) {
    auto acceptor = std::make_unique<rpc::Acceptor>(
        port, &loop_,
        [this](const rpc::Frame& frame, rpc::Connection* conn) { OnServerFrame(frame, conn); },
        [this](int fd) { OnPlayerDisconnected(fd); });

    loop_.Register(std::move(acceptor), EPOLLIN | EPOLLRDHUP | EPOLLET);
    printf("[GameService] 服务启动: port=%u\n", port);
    loop_.Run();
}

void GameService::Stop() {
    if (world_ && scenes_) {
        auto snap = world_->ExportScene("default");
        auto r = scenes_->Upsert(snap);
        if (!r.ok) {
            printf("[GameService] WARN: stop scene save failed: %s\n", r.error_msg.c_str());
        }
    }
    if (db_worker_)
        db_worker_->Stop();
    loop_.Stop();
}

} // namespace game
