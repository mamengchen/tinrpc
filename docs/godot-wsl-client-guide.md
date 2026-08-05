# Godot 4（Windows）接入 WSL tinrpc 指南

面向：**Windows 上的 Godot 4 客户端** ↔ **WSL2 里的 tinrpc 服务端**。  
范围：连上 TCP、跑通协议帧、完成 `Login` / `Move` 与默认世界推送。

相关设计：[登录与移动状态同步](superpowers/specs/2026-08-05-godot-login-move-design.md) · 协议契约：[`proto/game.proto`](../proto/game.proto)

---

## 1. 环境关系

```
┌────────────── Windows ──────────────┐          ┌────────── WSL2 (NAT) ──────────┐
│  Godot 4                             │  TCP     │  ./rpc                          │
│  StreamPeerTCP → WSL_IP:8080         │ ───────► │  监听 0.0.0.0:8080              │
│  帧编解码 + Protobuf body            │          │  GameService / WorldService     │
└──────────────────────────────────────┘          └────────────────────────────────┘
```

| 项 | 值 |
|----|-----|
| 传输 | TCP（非 HTTP / 非 WebSocket） |
| 默认端口 | `8080` |
| 监听地址 | `0.0.0.0`（所有网卡，含 eth0） |
| 当前本机 WSL 网络模式 | **NAT**（`wslinfo --networking-mode` → `nat`） |

**NAT 下 Windows 不能用 `127.0.0.1` 连 WSL 里的服务。**  
客户端必须连 **WSL 的 eth0 IPv4**（形如 `172.x.x.x`）。

若以后改成 **mirrored** 网络模式，可用 `127.0.0.1:8080`；本文以 NAT 为准。

---

## 2. WSL 侧：启动服务端

### 2.1 构建（首次或改代码后）

```bash
cd ~/mmo/tinrpc   # 按你的实际路径
mkdir -p build && cd build
cmake .. && make -j$(nproc) rpc
```

产物：`build/rpc`。

### 2.2 启动

```bash
cd ~/mmo/tinrpc/build
./rpc
```

正常日志类似：

```text
[GameService] 服务启动: port=8080
```

确认在听：

```bash
ss -tlnp | grep 8080
# 期望：0.0.0.0:8080  LISTEN  ... ("rpc",...)
```

### 2.3 查出 Windows 要连的 IP

```bash
hostname -I
# 或
ip -4 addr show eth0
```

取 eth0 的 `inet`，例如 `172.23.220.135`。  
**WSL 重启后该 IP 可能变化**，Godot 里的 host 要跟着改。

可选：在 WSL 里写到文件方便 Windows 读：

```bash
hostname -I | awk '{print $1}' > /mnt/c/temp/tinrpc_host.txt
```

---

## 3. Windows 侧：连通性

在 **PowerShell**（不是 WSL）：

```powershell
# 把 IP 换成你查到的
Test-NetConnection -ComputerName 172.23.220.135 -Port 8080
```

`TcpTestSucceeded : True` 即可进入 Godot。

### 连不上时

1. WSL 里 `./rpc` 是否在跑、`ss` 是否看到 `0.0.0.0:8080`
2. Godot / 测试是否误用了 `127.0.0.1`（NAT 下必失败）
3. IP 是否过期（重启 WSL 后重查）
4. Windows 防火墙：若本机其它软件拦出站，可临时放行 Godot，或先用 `Test-NetConnection` 排除
5. 公司 VPN / 多网卡：确认访问的是 WSL 虚拟网卡对应的 `172.x`

### 可选：mirrored 模式

在 `%UserProfile%\.wslconfig`：

```ini
[wsl2]
networkingMode=mirrored
```

然后 `wsl --shutdown` 再开。此后 Windows 可用 `127.0.0.1:8080`。  
改完再确认：`wslinfo --networking-mode`。

---

## 4. 协议：信封 + 信纸

客户端与服务端共享两层：

1. **信封**：tinrpc 协议帧（固定 13 字节头 + 方法名 + body）
2. **信纸**：Protobuf body，唯一契约为 `tinrpc/proto/game.proto`

### 4.1 帧布局（全部多字节字段为**网络字节序 / 大端**）

```
┌─────────┬──────────┬───────────┬───────────┬──────────┬──────────┬──────┐
│ 魔数     │ 总长度    │ 请求 ID    │ 消息类型    │ 方法名长度 │ 方法名     │ body │
│ 2 bytes  │ 4 bytes   │ 4 bytes    │ 1 byte     │ 2 bytes   │ N bytes   │ M    │
└─────────┴──────────┴───────────┴───────────┴──────────┴──────────┴──────┘
 ←──────────── 固定头 13 字节 ─────────────→
```

| 字段 | 说明 |
|------|------|
| 魔数 | 固定 `0xBABE` |
| 总长度 | `13 + N + M`（整帧字节数） |
| 请求 ID | 客户端单调递增；**服务端推送用 `0`** |
| 消息类型 | 见下表 |
| 方法名 | UTF-8，如 `"Login"`、`"Move"` |
| body | Protobuf 序列化结果；可为空 |

**MessageType**

| 值 | 含义 |
|----|------|
| `0x01` | Request（客户端 → 服务端） |
| `0x02` | Response（服务端 → 客户端；**推送也用这个**） |
| `0x03` | Error |

粘包处理：先读满至少 6 字节拿到 magic + total_len，再读满 `total_len` 字节得到一帧；缓冲区里可能有多帧或半帧。

### 4.2 本闭环用到的 RPC / 推送

| 方向 | 方法名 | Body | 说明 |
|------|--------|------|------|
| C→S | `Login` | `LoginReq` | `token` 即 `player_id`（当前实现） |
| S→C | `Login` | `LoginRes` | 同 `request_id` 的 Response |
| S→C | `WorldStateNtf` | `WorldStateNtf` | Login 后全量快照（`request_id=0`） |
| S→C | `PlayerEnterNtf` | `PlayerTransform` | 他人进入 |
| C→S | `Move` | `MoveReq` | 位置 + yaw（度） |
| S→C | `Move` | `MoveRes` | 成功或 `corrected_position` |
| S→C | `WorldStateNtf` | 变更玩家 | Move 后广播（节流） |
| S→C | `PlayerLeaveNtf` | `WorldPlayerLeaveNtf` | 断线离开 |

登录后数据流：

```
Godot                         tinrpc
  │── Login(token) ─────────────►│
  │◄─ LoginRes ──────────────────│  (匹配 request_id)
  │◄─ WorldStateNtf(全量) ───────│  (request_id = 0)
  │                              │── PlayerEnterNtf ──► 其他客户端
```

收包路由建议：

```
解码帧 →
  pending 里有该 request_id → 完成对应 Call（Login / Move）
  否则按 method_name 当推送：WorldStateNtf / PlayerEnterNtf / PlayerLeaveNtf
```

---

## 5. Godot 4 工程要点

建议独立工程（可放仓库 `godot_client/`），**proto 只维护一份**：权威路径 `tinrpc/proto/game.proto`（复制或 symlink 到工程内）。

```
godot_client/
  project.godot
  scripts/net/
    frame_codec.gd      # Encode / Decode + 粘包
    rpc_client.gd       # StreamPeerTCP、request_id、Call、notify
  scripts/game/
    net_session.gd      # Login / 推送处理
    player_spawner.gd   # 本地 / 远端角色
  proto/
    game.proto          # → 指向 tinrpc 同一份
```

### 5.1 连接配置

在导出变量或配置里写：

```gdscript
@export var host: String = "172.23.220.135"  # 换成你的 hostname -I
@export var port: int = 8080
@export var token: String = "player_a"       # 第二客户端用不同 token
```

### 5.2 帧编码（与服务端一致）

`StreamPeerBuffer` 务必 `big_endian = true`：

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

# decode_one(recv_buf) -> {ok, frame, consumed}
# 先校验 magic==0xBABE，再按 total_len 切一帧
```

### 5.3 RpcClient 行为

- `StreamPeerTCP.connect_to_host(host, port)`，每帧 `poll()` + 读入字节追加到缓冲
- `call_rpc(method, body)`：分配递增 `request_id`（从 1 起），发 `TYPE_REQUEST`，挂起等待同 id 的 Response/Error
- 无 pending 的帧：`emit_signal("notify", method_name, body)`（推送）

### 5.4 Protobuf

任选其一：

- Godot Protobuf 插件，从 `game.proto` 生成
- 本闭环字段少：可对手写 `LoginReq` / `MoveReq` 等最小编解码（字段号必须与 proto 一致）

最小验证可先发**合法序列化**的 `LoginReq{ token = "..." }`；body 乱填会导致服务端回 Error 帧。

### 5.5 最小验收

1. WSL：`./rpc`
2. Windows Godot 实例 A：`token=player_a`，连 `WSL_IP:8080`，Login 成功并收到 `WorldStateNtf`
3. 实例 B：`token=player_b`，Login 后 A 收到 `PlayerEnterNtf`
4. A 发 `Move`，B 看到位置更新
5. 关掉 A，B 收到 `PlayerLeaveNtf`

---

## 6. 排错清单

| 现象 | 排查 |
|------|------|
| `connect` 失败 / 超时 | NAT 下是否用了 `127.0.0.1`；IP 是否过期；`./rpc` 是否在听 `0.0.0.0:8080` |
| `Test-NetConnection` 失败 | 服务未起；防火墙；VPN |
| 连上后无响应 | 帧是否大端；magic 是否 `0xBABE`；`total_len` 是否含整帧 |
| Login 回 Error | body 不是合法 `LoginReq` Protobuf |
| 只有 LoginRes、没有 WorldStateNtf | 推送 `request_id=0`，勿当 RPC 超时丢掉；按 method 路由 |
| 第二客户端看不到第一人 | 是否用了相同 `token`（会顶掉映射）；是否处理 `PlayerEnterNtf` |
| Move 被拉回 | 超速 / 越界，看 `MoveRes.corrected_position` |

服务端侧可看登录日志：

```text
[GameService] 玩家登录: player_a
```

---

## 7. 快速对照

```bash
# WSL
cd ~/mmo/tinrpc/build && ./rpc
hostname -I | awk '{print $1}'
```

```powershell
# Windows
Test-NetConnection -ComputerName <WSL_IP> -Port 8080
```

Godot：`host=<WSL_IP>`，`port=8080`，TCP + 帧 `0xBABE` + `game.proto`，先 `Login` 再 `Move`。
