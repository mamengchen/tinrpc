# Godot Login + Move State Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 tinrpc 上实现默认世界登录与 3D 位置状态同步，并用 C++ 端到端测试验证；同时落地 Godot 4 最小客户端能连上同一协议。

**Architecture:** 扩展 `proto/game.proto`；新增无房间依赖的 `WorldService`（玩家表 + Move 校验/节流/广播）；`GameService` 在 Login/断连时接入世界，并在 `OnServerFrame` 中处理 `Move`（因需要 `fd→player_id`）。Godot 侧实现 tinrpc 帧编解码 + 手写少量 protobuf 字段编解码。

**Tech Stack:** C++20、CMake、Protobuf、tinrpc EventLoop/Dispatch、Godot 4 GDScript/`StreamPeerTCP`

**Spec:** `docs/superpowers/specs/2026-08-05-godot-login-move-design.md`

---

## File map

| 路径 | 职责 |
|------|------|
| `proto/game.proto` | 追加 Vec3 / PlayerTransform / Move* / Ntf |
| `include/game/world_service.h` | 默认世界 API |
| `src/game/world_service.cpp` | Enter/Leave/TryMove/广播节流 |
| `include/game/game_service.h` | 持有 WorldService |
| `src/game/game_service.cpp` | Login 进世界、Move、断连 Leave |
| `CMakeLists.txt` | 链入 world_service + 新测试 |
| `tests/test_world_service.cpp` | WorldService 单测 |
| `tests/test_world_e2e.cpp` | Login/Move/Leave 网络端到端 |
| `godot_client/` | Godot 4 最小客户端（帧 + proto + 会话） |

常量（实现写死）：

- `kMaxSpeed = 10.0f`（米/秒）
- 世界 AABB：`[-500, 500]` 各轴
- 出生点：`(0,0,0)`，`yaw = 0`（度）
- 广播节流：同玩家间隔 ≥ `50ms`（≤20Hz）

---

### Task 1: 扩展 `game.proto`

**Files:**
- Modify: `proto/game.proto`（文件末尾、`EchoRequest` 之前追加）

- [ ] **Step 1: 追加消息定义**

在 `// ---- 以下为序列化对比测试保留 ----` 之前插入：

```protobuf
// ---- 默认世界 / 状态同步（Godot） ----

message Vec3 {
    float x = 1;
    float y = 2;
    float z = 3;
}

message PlayerTransform {
    string player_id   = 1;
    string player_name = 2;
    Vec3   position    = 3;
    float  yaw         = 4;  // degrees
}

message MoveReq {
    Vec3  position       = 1;
    float yaw            = 2;
    int64 client_time_ms = 3;
}

message MoveRes {
    bool   success             = 1;
    string error_msg           = 2;
    Vec3   corrected_position  = 3;
}

message PlayerLeaveNtf {
    string player_id = 1;
}

message WorldStateNtf {
    repeated PlayerTransform players = 1;
}

// PlayerEnterNtf 直接使用 PlayerTransform 作为 body
```

- [ ] **Step 2: 重新配置并编译，确认 proto 生成**

```bash
cd /home/mamengchen/mmo/tinrpc/build
cmake .. && make -j$(nproc) rpc_lib
```

Expected: 成功；`build/game.pb.h` 含 `Vec3`、`MoveReq`、`WorldStateNtf` 等。

- [ ] **Step 3: Commit**

```bash
cd /home/mamengchen/mmo/tinrpc
git add proto/game.proto
git commit -m "$(cat <<'EOF'
feat(proto): add default-world move and snapshot messages

EOF
)"
```

---

### Task 2: WorldService 单测（先写失败用例）

**Files:**
- Create: `tests/test_world_service.cpp`
- Modify: `CMakeLists.txt`（添加 `test_world_service` 可执行文件）
- Create（下一步）: `include/game/world_service.h`, `src/game/world_service.cpp`

- [ ] **Step 1: 在 CMakeLists.txt 末尾追加测试目标**

```cmake
add_executable(test_world_service
    tests/test_world_service.cpp
)
target_link_libraries(test_world_service rpc_lib pthread)
```

同时把 `src/game/world_service.cpp` 加入 `rpc_lib` 的源列表（与 `game_service.cpp` 同级）。若文件尚不存在，先创建空实现桩，否则 CMake/链接会失败——本 Task Step 3 写最小桩。

- [ ] **Step 2: 编写 `tests/test_world_service.cpp`**

```cpp
#include "game/world_service.h"
#include "game.pb.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>

static int g_passed = 0, g_failed = 0;
void RunTest(const char* name, void (*fn)()) {
    printf("  %-55s ... ", name);
    try { fn(); printf("[PASS]\n"); g_passed++; }
    catch (...) { printf("[FAIL]\n"); g_failed++; }
}

void TestEnterSnapshotAndNotify() {
    std::vector<std::pair<std::string, std::string>> sent; // player_id, method
    game::WorldService world([&](const std::string& to, const std::string& method,
                                 const std::vector<uint8_t>&) {
        sent.emplace_back(to, method);
    });

    world.Enter("a", "Alice", /*now_ms=*/1000);
    // a 应收到 WorldStateNtf（含自己）
    assert(!sent.empty());
    assert(sent.back().first == "a");
    assert(sent.back().second == "WorldStateNtf");
    sent.clear();

    world.Enter("b", "Bob", 1100);
    // b 收到 WorldStateNtf；a 收到 PlayerEnterNtf
    bool b_snap = false, a_enter = false;
    for (auto& p : sent) {
        if (p.first == "b" && p.second == "WorldStateNtf") b_snap = true;
        if (p.first == "a" && p.second == "PlayerEnterNtf") a_enter = true;
    }
    assert(b_snap && a_enter);
}

void TestMoveSpeedReject() {
    std::vector<std::pair<std::string, std::string>> sent;
    game::WorldService world([&](const std::string& to, const std::string& method,
                                 const std::vector<uint8_t>&) {
        sent.emplace_back(to, method);
    });
    world.Enter("a", "A", 1000);
    world.Enter("b", "B", 1000);
    sent.clear();

    // 1ms 内移动 100m → 超速
    auto res = world.TryMove("a", 100.f, 0.f, 0.f, 0.f, /*now_ms=*/1001);
    assert(!res.success);
    assert(res.corrected_x == 0.f);
    // 不应向 b 广播
    for (auto& p : sent) {
        assert(!(p.first == "b" && p.second == "WorldStateNtf"));
    }
}

void TestMoveBroadcastAndLeave() {
    std::vector<std::pair<std::string, std::string>> sent;
    game::WorldService world([&](const std::string& to, const std::string& method,
                                 const std::vector<uint8_t>&) {
        sent.emplace_back(to, method);
    });
    world.Enter("a", "A", 1000);
    world.Enter("b", "B", 1000);
    sent.clear();

    auto res = world.TryMove("a", 1.f, 0.f, 0.f, 90.f, /*now_ms=*/1200); // 0.2s * 10m/s ok
    assert(res.success);
    bool b_got = false;
    for (auto& p : sent) {
        if (p.first == "b" && p.second == "WorldStateNtf") b_got = true;
    }
    assert(b_got);

    sent.clear();
    world.Leave("a");
    bool leave = false;
    for (auto& p : sent) {
        if (p.first == "b" && p.second == "PlayerLeaveNtf") leave = true;
    }
    assert(leave);
    assert(!world.HasPlayer("a"));
}

int main() {
    printf("=== test_world_service ===\n");
    RunTest("TestEnterSnapshotAndNotify", TestEnterSnapshotAndNotify);
    RunTest("TestMoveSpeedReject", TestMoveSpeedReject);
    RunTest("TestMoveBroadcastAndLeave", TestMoveBroadcastAndLeave);
    printf("Result: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
```

- [ ] **Step 3: 添加可编译的最小桩（使测试能链上并失败在 assert）**

创建 `include/game/world_service.h`：

```cpp
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
```

创建 `src/game/world_service.cpp` 空实现（全部 `assert(false)` 或空操作），例如：

```cpp
#include "game/world_service.h"
void game::WorldService::Enter(const std::string&, const std::string&, int64_t) {}
void game::WorldService::Leave(const std::string&) {}
game::MoveApplyResult game::WorldService::TryMove(const std::string&, float, float, float, float,
                                                    int64_t) {
    return {};
}
bool game::WorldService::HasPlayer(const std::string&) const { return false; }
void game::WorldService::SendWorldStateTo(const std::string&) {}
void game::WorldService::BroadcastEnter(const Player&) {}
void game::WorldService::BroadcastLeave(const std::string&) {}
void game::WorldService::BroadcastPose(const Player&, int64_t, bool) {}
```

把 `src/game/world_service.cpp` 加入 `CMakeLists.txt` 的 `rpc_lib` sources。

- [ ] **Step 4: 编译并跑测试，确认失败**

```bash
cd /home/mamengchen/mmo/tinrpc/build
cmake .. && make -j$(nproc) test_world_service
./test_world_service
```

Expected: 编译通过；测试 FAIL（Enter/Move assert）。

- [ ] **Step 5: Commit 测试与桩**

```bash
git add include/game/world_service.h src/game/world_service.cpp tests/test_world_service.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
test: add failing WorldService unit tests and stubs

EOF
)"
```

---

### Task 3: 实现 WorldService

**Files:**
- Modify: `src/game/world_service.cpp`（完整实现）
- Modify: `include/game/world_service.h`（如需私有辅助保持不变则跳过）

- [ ] **Step 1: 实现 Enter / Leave / TryMove / 广播**

`src/game/world_service.cpp` 要点：

```cpp
#include "game/world_service.h"
#include "game.pb.h"
#include <algorithm>
#include <cmath>

namespace game {
namespace {

PlayerTransform ToTransform(const WorldService::Player& /* need friend or duplicate fields */) {
  // 在 cpp 内用局部逻辑填充 PlayerTransform
}

float Dist(float x1, float y1, float z1, float x2, float y2, float z2) {
    float dx = x1 - x2, dy = y1 - y2, dz = z1 - z2;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Clamp(float& x, float& y, float& z) {
    auto c = [](float v) { return std::max(-WorldService::kBound, std::min(WorldService::kBound, v)); };
    x = c(x); y = c(y); z = c(z);
}

} // namespace

void WorldService::Enter(const std::string& player_id, const std::string& name, int64_t now_ms) {
    Player p;
    p.player_id = player_id;
    p.name = name;
    p.x = p.y = p.z = 0;
    p.yaw = 0;
    p.last_move_ms = now_ms;
    p.last_broadcast_ms = 0;
    players_[player_id] = p;
    SendWorldStateTo(player_id);
    BroadcastEnter(players_[player_id]);
}

void WorldService::Leave(const std::string& player_id) {
    if (!players_.erase(player_id)) return;
    BroadcastLeave(player_id);
}

MoveApplyResult WorldService::TryMove(const std::string& player_id, float x, float y, float z,
                                      float yaw, int64_t now_ms) {
    MoveApplyResult out;
    auto it = players_.find(player_id);
    if (it == players_.end()) {
        out.error_msg = "not in world";
        return out;
    }
    Player& p = it->second;
    float nx = x, ny = y, nz = z;
    Clamp(nx, ny, nz);
    bool clamped = (nx != x || ny != y || nz != z);

    int64_t dt = std::max<int64_t>(now_ms - p.last_move_ms, 1);
    float max_dist = kMaxSpeed * (static_cast<float>(dt) / 1000.0f);
    float d = Dist(p.x, p.y, p.z, nx, ny, nz);
    if (d > max_dist + 1e-3f || clamped) {
        out.success = false;
        out.error_msg = clamped ? "out of bounds" : "too fast";
        out.corrected_x = p.x;
        out.corrected_y = p.y;
        out.corrected_z = p.z;
        return out;
    }
    p.x = nx; p.y = ny; p.z = nz; p.yaw = yaw;
    p.last_move_ms = now_ms;
    out.success = true;
    out.corrected_x = p.x; out.corrected_y = p.y; out.corrected_z = p.z;
    BroadcastPose(p, now_ms, /*force=*/false);
    return out;
}

bool WorldService::HasPlayer(const std::string& id) const {
    return players_.count(id) > 0;
}

void WorldService::SendWorldStateTo(const std::string& to) {
    WorldStateNtf ntf;
    for (auto& [_, p] : players_) {
        auto* t = ntf.add_players();
        t->set_player_id(p.player_id);
        t->set_player_name(p.name);
        t->mutable_position()->set_x(p.x);
        t->mutable_position()->set_y(p.y);
        t->mutable_position()->set_z(p.z);
        t->set_yaw(p.yaw);
    }
    std::string buf;
    ntf.SerializeToString(&buf);
    send_(to, "WorldStateNtf", std::vector<uint8_t>(buf.begin(), buf.end()));
}

void WorldService::BroadcastEnter(const Player& p) {
    PlayerTransform t;
    t.set_player_id(p.player_id);
    t.set_player_name(p.name);
    t.mutable_position()->set_x(p.x);
    t.mutable_position()->set_y(p.y);
    t.mutable_position()->set_z(p.z);
    t.set_yaw(p.yaw);
    std::string buf;
    t.SerializeToString(&buf);
    auto body = std::vector<uint8_t>(buf.begin(), buf.end());
    for (auto& [id, _] : players_) {
        if (id == p.player_id) continue;
        send_(id, "PlayerEnterNtf", body);
    }
}

void WorldService::BroadcastLeave(const std::string& player_id) {
    PlayerLeaveNtf ntf;
    ntf.set_player_id(player_id);
    std::string buf;
    ntf.SerializeToString(&buf);
    auto body = std::vector<uint8_t>(buf.begin(), buf.end());
    for (auto& [id, _] : players_) {
        send_(id, "PlayerLeaveNtf", body);
    }
}

void WorldService::BroadcastPose(const Player& p, int64_t now_ms, bool force) {
    auto it = players_.find(p.player_id);
    if (it == players_.end()) return;
    if (!force && it->second.last_broadcast_ms > 0 &&
        now_ms - it->second.last_broadcast_ms < kMinBroadcastIntervalMs) {
        return;
    }
    it->second.last_broadcast_ms = now_ms;

    WorldStateNtf ntf;
    auto* t = ntf.add_players();
    t->set_player_id(p.player_id);
    t->mutable_position()->set_x(p.x);
    t->mutable_position()->set_y(p.y);
    t->mutable_position()->set_z(p.z);
    t->set_yaw(p.yaw);
    std::string buf;
    ntf.SerializeToString(&buf);
    auto body = std::vector<uint8_t>(buf.begin(), buf.end());
    for (auto& [id, _] : players_) {
        if (id == p.player_id) continue;
        send_(id, "WorldStateNtf", body);
    }
}

} // namespace game
```

注意：头文件里 `Player` 是 private，cpp 中 `BroadcastEnter(const Player&)` 合法。若编译器对嵌套类型访问有问题，把辅助函数都做成成员函数内联实现。

- [ ] **Step 2: 跑单测通过**

```bash
cd /home/mamengchen/mmo/tinrpc/build && make -j$(nproc) test_world_service && ./test_world_service
```

Expected: `3 passed, 0 failed`

- [ ] **Step 3: Commit**

```bash
git add include/game/world_service.h src/game/world_service.cpp
git commit -m "$(cat <<'EOF'
feat: implement WorldService enter/move/leave with speed checks

EOF
)"
```

---

### Task 4: 接入 GameService（Login / Move / Disconnect）

**Files:**
- Modify: `include/game/game_service.h`
- Modify: `src/game/game_service.cpp`

- [ ] **Step 1: 头文件持有 WorldService**

在 `game_service.h` 增加 `#include "game/world_service.h"`，成员：

```cpp
std::unique_ptr<WorldService> world_;
```

在 private 区增加：

```cpp
void HandleMove(const rpc::Frame& frame, rpc::Connection* conn);
int64_t NowMs() const;
void SendToPlayer(const std::string& player_id, const std::string& method,
                  const std::vector<uint8_t>& body);
```

- [ ] **Step 2: 构造函数里创建 WorldService**

在 `GameService::GameService()` 中，`broadcast_` 创建之后：

```cpp
world_ = std::make_unique<WorldService>(
    [this](const std::string& player_id, const std::string& method,
           const std::vector<uint8_t>& body) { SendToPlayer(player_id, method, body); });
```

实现：

```cpp
int64_t GameService::NowMs() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void GameService::SendToPlayer(const std::string& player_id, const std::string& method,
                               const std::vector<uint8_t>& body) {
    auto it = player_conns_.find(player_id);
    if (it == player_conns_.end() || !it->second) return;
    auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, method, body);
    it->second->Send(frame);
}
```

- [ ] **Step 3: 增强 Login 分支**

在现有 `RegisterPlayerConn` 与发送 `LoginRes` 之后调用：

```cpp
world_->Enter(req.token(), req.token(), NowMs());
```

顺序必须是：先 `RegisterPlayerConn`，再 `Enter`（Enter 会立刻 `SendToPlayer`）。

- [ ] **Step 4: 处理 Move（不要走无 conn 上下文的 Dispatch）**

在 `OnServerFrame` 的 Login 分支之后、`dispatch_.Call` 之前：

```cpp
if (frame.method_name == "Move") {
    HandleMove(frame, conn);
    return;
}
```

`HandleMove`：

```cpp
void GameService::HandleMove(const rpc::Frame& frame, rpc::Connection* conn) {
    auto it = fd_to_player_.find(conn->GetFd());
    MoveRes res;
    if (it == fd_to_player_.end()) {
        res.set_success(false);
        res.set_error_msg("not logged in");
    } else {
        MoveReq req;
        if (!req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()))) {
            auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                  "Move", {});
            conn->Send(err);
            metrics_.OnError();
            return;
        }
        float x = req.position().x(), y = req.position().y(), z = req.position().z();
        auto apply = world_->TryMove(it->second, x, y, z, req.yaw(), NowMs());
        res.set_success(apply.success);
        res.set_error_msg(apply.error_msg);
        res.mutable_corrected_position()->set_x(apply.corrected_x);
        res.mutable_corrected_position()->set_y(apply.corrected_y);
        res.mutable_corrected_position()->set_z(apply.corrected_z);
    }
    std::string buf;
    res.SerializeToString(&buf);
    auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response, "Move",
                                          std::vector<uint8_t>(buf.begin(), buf.end()));
    conn->Send(rsp);
    metrics_.OnRequest(0); // 或沿用现有 latency 计算
}
```

- [ ] **Step 5: 断连时 Leave**

在 `OnPlayerDisconnected` 中，房间清理之前或之后（建议连接清理前）：

```cpp
world_->Leave(player_id);
```

- [ ] **Step 6: 编译主库与 rpc**

```bash
cd /home/mamengchen/mmo/tinrpc/build && make -j$(nproc) rpc test_world_service
```

Expected: 成功。

- [ ] **Step 7: Commit**

```bash
git add include/game/game_service.h src/game/game_service.cpp
git commit -m "$(cat <<'EOF'
feat: wire WorldService into Login, Move, and disconnect

EOF
)"
```

---

### Task 5: 网络端到端测试 `test_world_e2e`

**Files:**
- Create: `tests/test_world_e2e.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 添加 CMake 目标**

```cmake
add_executable(test_world_e2e
    tests/test_world_e2e.cpp
)
target_link_libraries(test_world_e2e rpc_lib pthread)
```

- [ ] **Step 2: 编写端到端测试**

模式复制 `tests/test_room_service.cpp` 的 `SimpleClient`（Connect / SendFrame / RecvFrame）。服务端启动方式：后台线程跑 `GameService::Run(port)`，测完 `Stop()`。

覆盖：

1. `A` Login → 收到 `LoginRes` + `WorldStateNtf`（含自己）  
2. `B` Login → `A` 收到 `PlayerEnterNtf`；`B` 收到含两人的 `WorldStateNtf`  
3. `A` Move 合法位移 → `MoveRes.success`；`B` 收到 `WorldStateNtf`  
4. `A` 关闭 socket → `B` 收到 `PlayerLeaveNtf`

关键断言示例：

```cpp
// Login
game::LoginReq req; req.set_token("player_a");
auto body = Serialize(req);
client.Call("Login", body); // 或手写 Encode Request + 读 Response
// 然后非阻塞/超时读 push：method == "WorldStateNtf"
```

推送帧的 `request_id == 0`，`msg_type == Response`。

- [ ] **Step 3: 运行**

```bash
cd /home/mamengchen/mmo/tinrpc/build
make -j$(nproc) test_world_e2e
./test_world_e2e
```

Expected: 全部 PASS。

- [ ] **Step 4: Commit**

```bash
git add tests/test_world_e2e.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
test: add world login/move/leave end-to-end coverage

EOF
)"
```

---

### Task 6: Godot 4 — 帧编解码与 RPC 客户端

**Files:**
- Create: `godot_client/project.godot`（最小工程）
- Create: `godot_client/scripts/net/frame_codec.gd`
- Create: `godot_client/scripts/net/rpc_client.gd`
- Create: `godot_client/proto/README.md`（说明权威 proto 路径为仓库根 `proto/game.proto`）

- [ ] **Step 1: 实现 `frame_codec.gd`（与 `src/protocol.cpp` 一致）**

```gdscript
extends RefCounted
class_name FrameCodec

const MAGIC := 0xBABE
const HEADER_SIZE := 13
const TYPE_REQUEST := 0x01
const TYPE_RESPONSE := 0x02
const TYPE_ERROR := 0x03

static func encode(request_id: int, msg_type: int, method: String, body: PackedByteArray) -> PackedByteArray:
    var method_buf := method.to_utf8_buffer()
    var total_len := HEADER_SIZE + method_buf.size() + body.size()
    var out := PackedByteArray()
    out.resize(total_len)
    var s := StreamPeerBuffer.new()
    s.big_endian = true
    s.put_u16(MAGIC)
    s.put_u32(total_len)
    s.put_u32(request_id)
    s.put_u8(msg_type)
    s.put_u16(method_buf.size())
    s.put_data(method_buf)
    s.put_data(body)
    return s.data_array

# decode_one(buffer) -> {ok, frame, consumed} 实现粘包：先读 6 字节(magic+len)再读满 total_len
```

- [ ] **Step 2: 实现 `rpc_client.gd`**

- `StreamPeerTCP` 连接 `host:port`  
- `call_rpc(method, body) -> body`：递增 `request_id`，挂起等待匹配 Response  
- `_process`/`poll`：读入字节 → `FrameCodec` 拆帧 → 若有 pending 则完成，否则 `emit_signal("notify", method, body)`

- [ ] **Step 3: 在 Windows Godot 4 打开工程，对本地 WSL IP:8080 做一次空 Login（body 可先硬编码）验证 TCP+帧**

WSL IP 查询：`ip addr show eth0`。

- [ ] **Step 4: Commit**

```bash
git add godot_client/
git commit -m "$(cat <<'EOF'
feat(godot): add tinrpc frame codec and TCP rpc client skeleton

EOF
)"
```

---

### Task 7: Godot — 手写最小 protobuf + 登录移动会话

**Files:**
- Create: `godot_client/scripts/net/proto_bridge.gd`
- Create: `godot_client/scripts/game/net_session.gd`
- Create: `godot_client/scripts/game/player_spawner.gd`
- Create: `godot_client/scenes/main.tscn`（简单地平面 + 两个占位 Mesh）

Protobuf wire 手写范围（仅本功能用到的字段）：

| 消息 | 字段 |
|------|------|
| LoginReq | 1: string token |
| LoginRes | 1: bool success, 2: PlayerInfo(player_id=1) |
| MoveReq | 1: Vec3, 2: float yaw |
| MoveRes | 1: bool, 3: Vec3 corrected |
| PlayerTransform | 1 id, 2 name, 3 Vec3, 4 yaw |
| WorldStateNtf | 1 repeated PlayerTransform |
| PlayerLeaveNtf | 1 string id |

`proto_bridge.gd` 提供 `encode_login_req` / `decode_login_res` / `encode_move_req` / `decode_world_state` 等。

- [ ] **Step 1: 实现 encode/decode（可用最小 protobuf 库或按 wire format 手写 varint/length-delimited）**

若手写成本过高：在 Godot 用 [protobuf addon](https://github.com/onikun94/godot-protobuf) 从 `proto/game.proto` 生成，仍以仓库 `proto/game.proto` 为唯一源。

- [ ] **Step 2: `net_session.gd` 流程**

```
connect → Login(token) → 处理 WorldStateNtf/PlayerEnterNtf/PlayerLeaveNtf
每物理帧或定时：本地位置变化则 Call Move
MoveRes.success==false → 拉回 corrected_position
```

- [ ] **Step 3: 双客户端验收（spec §7）**

启动：`./rpc`（WSL）。两个 Godot 实例不同 token，验证出现/移动/断开。

- [ ] **Step 4: Commit**

```bash
git add godot_client/
git commit -m "$(cat <<'EOF'
feat(godot): login, move sync, and player spawn for default world

EOF
)"
```

---

### Task 8: 文档同步与推送

**Files:**
- Modify: `README.md`（简短增加「默认世界 / Godot」小节，链到 spec）
- Optional: `docs/devlog.md` 一行记录

- [ ] **Step 1: README 增加启动说明**

```markdown
### Godot 默认世界（状态同步）

协议见 `docs/superpowers/specs/2026-08-05-godot-login-move-design.md`。
服务端：`./rpc`。Godot 工程：`godot_client/`。
```

- [ ] **Step 2: 跑回归（至少世界相关）**

```bash
cd build
./test_world_service && ./test_world_e2e
```

- [ ] **Step 3: Commit + push 到 `mamengchen/tinrpc`**

```bash
git add README.md docs/
git commit -m "$(cat <<'EOF'
docs: document Godot default-world login/move flow

EOF
)"
git push origin main
```

---

## Spec coverage checklist

| Spec 项 | Task |
|---------|------|
| proto 消息 Vec3/Move/Ntf | Task 1 |
| WorldService 默认世界 | Task 2–3 |
| Login 进世界 + 快照/Enter | Task 4 |
| Move 校验/节流/广播 | Task 3–4 |
| 断连 Leave | Task 4–5 |
| C++ 验收替代路径 | Task 5 |
| Godot 帧+会话 | Task 6–7 |
| 非目标（房间/帧同步等） | 不实现 |

## Self-review notes

- Move 必须在 `OnServerFrame` 处理（需要 `fd_to_player_`），不能只 `RegisterMethod("Move")`。  
- `Enter` 前必须已 `RegisterPlayerConn`。  
- yaw 单位：度。  
- 无 TBD 占位；Godot protobuf 允许 addon 或手写二选一，以同一 `game.proto` 为源。
