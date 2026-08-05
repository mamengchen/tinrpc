# Phase 2：玩家状态 MongoDB 存档设计

日期：2026-08-05  
状态：实现中  
前置：Phase 1 账号认证已完成

## 范围

持久化每位玩家在默认世界中的：

- `position`：x, y, z  
- `yaw`  
- `wood` / `stone`（当前 WorldService 背包）

不持久化：资源点、建筑（Phase 3 场景状态）。

## 集合 `players`

- 库：与账号相同（`TINRPC_MONGO_DB`）  
- 主键：`player_id`（唯一索引）  
- 字段：`x,y,z,yaw,wood,stone,updated_at`

## 时机

1. **Login 成功**：DbWorker `Load` → IO 线程 `World.Enter` 使用存档（无档则默认出生点 + 默认背包）  
2. **断线 / Leave 前**：从 World 取快照 → 异步 `Upsert`  
3. 不做每帧写库；Move 不触发写库

## 模块

- `PlayerStore`：Load / Upsert（libmongoc，仅 DbWorker 调用）  
- `WorldService`：`Enter` 支持初始状态；`GetSnapshot`  
- `GameService`：Login/断线接线

## 验收

1. 登录 → 移动 → 改背包（若可）→ 断线 → 再登录，位置与背包恢复  
2. 新账号首次登录：出生 (0,0,0)、默认 wood/stone  
3. Mongo 不可用时：登录仍可用账号，进世界用默认状态（打日志）；断线存档失败不崩进程  
