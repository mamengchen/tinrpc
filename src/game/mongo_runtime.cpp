#include "game/mongo_runtime.h"

#include <mongoc/mongoc.h>

#include <mutex>

namespace game {
namespace {

std::once_flag g_mongoc_init_flag;

} // namespace

void EnsureMongocInit() {
    std::call_once(g_mongoc_init_flag, [] { mongoc_init(); });
}

} // namespace game
