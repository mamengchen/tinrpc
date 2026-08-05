#pragma once

namespace game {

/// 进程内只初始化一次 mongoc
void EnsureMongocInit();

} // namespace game
