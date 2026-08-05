# Phase 1：MongoDB 账号注册与登录设计

日期：2026-08-05  
状态：已确认  
范围：仅账号注册 / 登录持久化；玩家存档与场景状态见后续 Phase 2 / 3

相关：默认世界状态同步见 `2026-08-05-godot-login-move-design.md`

## 1. 目标

- 本机 `mongod` 存储账号
- tinrpc 内嵌 **mongo-cxx-driver**，经 **DbWorker** 访问，避免阻塞 epoll IO 线程
- 新增 `Register`；将 `Login` 从 token 改为 **用户名 + 密码**
- 登录成功后行为与现网一致：绑定连接 → 进入默认世界 → 推送 `WorldStateNtf`

## 2. 非目标（本期不做）

- 邮箱验证、OAuth、JWT / 会话表
- 玩家位置 / 属性持久化（Phase 2）
- 场景状态持久化（Phase 3）
- IO 线程内同步阻塞查库

## 3. 架构

```
Godot                    tinrpc (IO 线程)              DbWorker              mongod
  │── Register/Login ──►│ 校验格式 / 入队 ───────────►│ mongocxx + bcrypt ─►│ accounts
  │◄─ Res ──────────────│◄── 完成回调投回 EventLoop ──│                     │
  │                     │ Login OK → World.Enter       │                     │
```

| 模块 | 职责 |
|------|------|
| `MongoClient` | URI、数据库名、共享连接/池初始化 |
| `AccountStore` | CreateAccount、VerifyCredentials |
| `DbWorker` | 单线程任务队列，执行阻塞 DB/哈希 |
| `GameService` | `Register` / 改造后的 `Login`；成功后进世界 |

密码哈希：服务端 **bcrypt**（或等价）；Mongo 只存 `password_hash`。

## 4. 协议（破坏性变更）

旧 `LoginReq.token` 作废。

```protobuf
message RegisterReq {
  string username = 1;  // 3~32，[A-Za-z0-9_]
  string password = 2;  // 6~64
}

message RegisterRes {
  bool   success   = 1;
  string error_msg = 2;
  string player_id = 3;  // 成功时；Phase 1 等于 username
}

message LoginReq {
  string username = 1;  // 原 field 1 语义变更（曾为 token）
  string password = 2;
}

// LoginRes 字段编号不变
message LoginRes {
  bool success = 1;
  PlayerInfo player_info = 2;
  string error_msg = 3;
}
```

RPC 方法名：`Register`、`Login`。

Godot `proto_bridge` / 测试客户端需同步改字段；旧 token 登录客户端将不兼容。

## 5. MongoDB 模型

| 项 | 值 |
|----|-----|
| 默认 URI | `mongodb://127.0.0.1:27017` |
| 数据库 | `tinrpc`（测试可用 `tinrpc_test`） |
| 集合 | `accounts` |
| 索引 | `username` **唯一** |

文档：

```json
{
  "username": "alice",
  "password_hash": "$2b$...",
  "player_id": "alice",
  "created_at": {"$date": "..."},
  "updated_at": {"$date": "..."}
}
```

配置（环境变量，可选）：

- `TINRPC_MONGO_URI`
- `TINRPC_MONGO_DB`

## 6. 时序

### Register

1. IO：解析 `RegisterReq`，校验用户名/密码长度与字符集  
2. 投递 DbWorker：`bcrypt(password)` → `insert`  
3. 唯一键冲突 → `RegisterRes(success=false, error_msg="username taken")`  
4. 成功 → `RegisterRes` 带 `player_id`；**不**自动进世界  

### Login

1. IO：解析 `LoginReq`，投递 Verify  
2. DbWorker：`find` + bcrypt verify；用户不存在与密码错误统一 `invalid credentials`  
3. 成功（IO）：若连接上已有旧 `player_id`，先 `World.Leave` / 解绑；再 `RegisterPlayerConn` → `World.Enter` → `LoginRes`  
4. Mongo 不可用 → `db unavailable`（或 Error 帧），进程不退出  

## 7. 错误语义

| 场景 | 结果 |
|------|------|
| 用户名已存在 | `username taken` |
| 格式非法 | `invalid username/password` |
| 登录失败 | `invalid credentials` |
| DB 故障 | `db unavailable` |

## 8. 验收

1. 本机 `mongod` 运行中  
2. 同名注册第二次失败  
3. 错密码登录失败；正确密码成功并收到默认世界快照  
4. Godot：Register → Login → 进世界  
5. 停止 `mongod` 后 Register/Login 返回 DB 错误且服务不崩  

## 9. Phase 2 / 3 预留

- Phase 2：`players` 集合（位置、yaw 等），Login/断线时读写  
- Phase 3：`scenes` 集合（场景持久物件/进度）  
- 共用 `MongoClient` + `DbWorker`，不另起连接架构  

## 10. 依赖与运维备注

- 构建依赖：mongo-cxx-driver（及 bsoncxx）、bcrypt 库（或 openssl 自实现需在实现计划中选定）  
- 开发机：安装并启动 `mongod`；CI/本地测试指向 `tinrpc_test` 库并在套件前后清理 `accounts`
