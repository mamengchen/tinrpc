# Godot 4 + tinrpc：登录与移动状态同步设计

日期：2026-08-05  
状态：已确认  
范围：最小闭环（登录 → 默认世界 → 移动同步）；房间 / 匹配 / 帧同步不在本次

## 1. 目标与约束

- **客户端**：Godot 4（Windows）
- **服务端**：tinrpc（现有 C++20 / epoll / Protobuf）
- **同步模型**：状态同步（位置 + 朝向）；帧同步以后再接
- **世界模型**：Login 成功即进入**默认世界**；房间系统以后再加，本次不依赖 Create/JoinRoom
- **协议共享**：以 `proto/game.proto` 为唯一业务消息契约（方案 1）

客户端与服务端共享两层：

1. **信封**：tinrpc 协议帧（魔数 `0xBABE`、长度前缀、request_id、MessageType、方法名、body）
2. **信纸**：Protobuf body（本设计扩展的消息）

## 2. 架构

```
┌──────────────── Godot 4 ────────────────┐     TCP      ┌────────── tinrpc ──────────┐
│  游戏逻辑 (玩家/相机)                      │◄──────────►│  GameService               │
│       ↓↑                                  │   帧+Proto  │    ├─ Login（已有，增强）    │
│  RpcClient (request_id / 方法名)           │             │    ├─ WorldService（新增）   │
│       ↓↑                                  │             │    │    · 默认世界玩家表      │
│  FrameCodec (13B 信封 + 粘包)              │             │    │    · Move 校验+广播      │
│       ↓↑                                  │             │    └─ Broadcast / Dispatch  │
│  Protobuf (game.proto)                    │             │  proto/game.proto（唯一契约）│
└───────────────────────────────────────────┘             └────────────────────────────┘
```

原则：

- `.proto` 只描述 body；帧头两边各自实现，语义与现有 tinrpc 一致
- 默认世界与房间解耦；不改房间状态机
- 推送消息按**方法名**分发（`request_id = 0`），与现有 `RoomEvent` 模式一致

## 3. 协议扩展（`proto/game.proto`）

不修改已有字段编号；仅追加消息。

### 3.1 基础类型

```protobuf
message Vec3 {
  float x = 1;
  float y = 2;
  float z = 3;
}

message PlayerTransform {
  string player_id = 1;
  string player_name = 2;  // Enter / 快照时可带；Move 广播可省略 name
  Vec3   position = 3;
  float  yaw = 4;          // 水平朝向，单位：度（实现时写死并文档化）
}
```

### 3.2 RPC（Request / Response）

| 方法名 | 请求 | 响应 | 说明 |
|--------|------|------|------|
| `Login` | 已有 `LoginReq` | 已有 `LoginRes` | token → player_id；成功后进入默认世界 |
| `Move` | `MoveReq` | `MoveRes` | 客户端上报期望位置与朝向 |

```protobuf
message MoveReq {
  Vec3  position = 1;
  float yaw = 2;
  int64 client_time_ms = 3;  // 可选，调试/插值
}

message MoveRes {
  bool   success = 1;
  string error_msg = 2;
  Vec3   corrected_position = 3;  // 被拉回时填服务端权威位置
}
```

### 3.3 服务端推送（按方法名路由）

| 方法名 | 消息体 | 何时 |
|--------|--------|------|
| `PlayerEnterNtf` | `PlayerTransform` | 其他玩家进入默认世界时推送给已在线者 |
| `PlayerLeaveNtf` | body 用 `WorldPlayerLeaveNtf`（`player_id`） | 断线或离开默认世界；与房间版 `PlayerLeaveNtf` 消息类型区分 |
| `WorldStateNtf` | `repeated PlayerTransform` | Login 成功后发给自己（全量快照）；Move 后可广播变更（可只含变化的玩家） |

```protobuf
// 与房间 PlayerLeaveNtf 重名冲突，默认世界使用：
message WorldPlayerLeaveNtf {
  string player_id = 1;
}

message WorldStateNtf {
  repeated PlayerTransform players = 1;
}
```

### 3.4 Login 行为增强（可不改 message 字段）

1. 校验 / 解析 `LoginReq`（沿用现有：`token` 作为 `player_id`）
2. 注册 `player_id ↔ Connection`
3. 调用 `WorldService.Enter`（出生点默认 `(0,0,0)`，`yaw=0`）
4. 回 `LoginRes`
5. 给自己发 `WorldStateNtf`（当前默认世界内所有玩家，含自己）
6. 给其他在线玩家发 `PlayerEnterNtf`（仅新人）

## 4. 数据流

### 4.1 登录进世界

```
Godot                         tinrpc
  │── Login(token) ─────────────►│ 建立映射；写入默认世界
  │◄─ LoginRes ──────────────────│
  │◄─ WorldStateNtf(全量) ───────│
  │                              │── PlayerEnterNtf ──► 其他客户端
```

### 4.2 移动

```
Godot                         tinrpc
  │── Move(pos, yaw) ───────────►│ 速度 / 边界校验
  │◄─ MoveRes(ok / corrected) ───│
  │                              │── WorldStateNtf(该玩家) ──► 其他客户端
  │                              │   （节流：同玩家 ≤ 20Hz）
```

### 4.3 断线

```
TCP 断开 → WorldService.Leave → PlayerLeaveNtf → 清理 player_conns_
```

### 4.4 Godot 收包路由

```
帧解码 → method_name
  ├─ pending[request_id] 存在 → 完成对应 RPC（Login / Move）
  └─ 否则按推送：WorldStateNtf / PlayerEnterNtf / PlayerLeaveNtf
```

## 5. 服务端模块

| 模块 | 职责 |
|------|------|
| `proto/game.proto` | 追加本文第 3 节消息 |
| `WorldService`（新） | 默认世界玩家表；Enter / Leave；Move 校验、节流、广播 |
| `GameService` | Login 成功后 Enter；注册 `Move`；断连 Leave |
| 测试 | Login 快照；双人 Move 广播；断连 Leave |

复用现有 `player_conns_` 与发送路径；**不修改**房间 / 匹配 / 帧同步核心逻辑。

### 5.1 最小校验规则（明确）

- 未 Login（无 player 映射）的 `Move`：拒绝
- 位移相对上一权威位置超过 `max_speed * dt`（实现时给出常量，如 10 m/s）：`MoveRes.success=false`，填 `corrected_position`，**不**向他人广播脏位置
- 简单世界边界（如轴对齐盒子）：超出则钳制到边界内

## 6. Godot 4 侧结构

建议独立工程，可置于仓库 `godot_client/` 或仓库外；**协议文件只维护一份**：

- 权威路径：`tinrpc/proto/game.proto`
- Godot 通过复制或 symlink 引用同一文件

建议目录：

```
godot_client/
  scripts/net/
    frame_codec.gd      # tinrpc 帧 Encode/Decode + 粘包
    rpc_client.gd       # StreamPeerTCP、request_id、Call
    proto_bridge.gd     # game.proto 编解码封装
  scripts/game/
    net_session.gd      # Login 与推送处理
    player_spawner.gd   # 本地 / 远端角色与插值
  proto/
    game.proto          # 指向 tinrpc 同一份契约
```

Protobuf 接入：可用 Godot protobuf 插件，或对本次少量消息手写 / 生成最小编解码。帧层必须符合 tinrpc：`kProtocolMagic=0xBABE`、网络字节序、现有 `MessageType`。

## 7. 验收标准

1. 两个 Godot 客户端连接同一 tinrpc，使用不同 token 登录
2. 双方能看到对方进入默认世界
3. A 移动，B 能看到位置更新
4. A 断开，B 收到离开并销毁远端角色

## 8. 非目标（本次不做）

- 房间 / 匹配 / 帧同步接入移动
- 真实鉴权、账号持久化
- AOI / 兴趣管理
- 服务端物理权威模拟（仅速度与边界校验）
- 自动 `git push` 以外的部署流水线变更

## 9. 错误处理

- body 解析失败：对该请求回 `Error` 帧；推送则记录日志并忽略
- Move 超速 / 越界：`MoveRes` 失败 + `corrected_position`，客户端拉回
- 未 Login 调用 Move：拒绝
