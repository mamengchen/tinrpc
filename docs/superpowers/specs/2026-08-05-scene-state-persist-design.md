# Phase 3：默认场景状态持久化设计

日期：2026-08-05  
状态：实现中

## 范围

持久化默认世界的：

- 资源点：`id, type, x,y,z, remaining`
- 建筑：`id, owner_id, type, x,y,z, yaw`
- `next_building_id`

不持久化在线玩家（仍走 Phase 2 `players`）。

## 集合 `scenes`

单文档（`scene_id = "default"`）upsert：

```json
{
  "scene_id": "default",
  "resources": [...],
  "buildings": [...],
  "next_building_id": 1,
  "updated_at": ISODate
}
```

## 时机

1. **启动**：同步 Load；无档则 `SeedDefaultScene`
2. **Gather / PlaceBuilding 成功后**：异步 Upsert 全量场景快照
3. **进程 Stop**：再保存一次（尽力而为）

## 验收

1. 采集资源后重启服务，`remaining` 保持  
2. 放置建筑后重启，建筑仍在  
3. Mongo 不可用时仍能 Seed 默认场景开服  
