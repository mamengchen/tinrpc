#include "game/player_store.h"
#include "game/mongo_runtime.h"

#include <bson/bson.h>
#include <mongoc/mongoc.h>

#include <ctime>

namespace game {
namespace {

constexpr const char* kColl = "players";

} // namespace

PlayerStore::PlayerStore(std::string uri, std::string db_name)
    : uri_(std::move(uri)), db_name_(std::move(db_name)) {
    EnsureMongocInit();
}

PlayerStore::~PlayerStore() {
    if (client_) {
        mongoc_client_destroy(static_cast<mongoc_client_t*>(client_));
        client_ = nullptr;
    }
}

bool PlayerStore::Init(std::string* err) {
    bson_error_t error;
    mongoc_client_t* client = mongoc_client_new(uri_.c_str());
    if (!client) {
        if (err)
            *err = "invalid mongo uri";
        return false;
    }

    bson_t ping = BSON_INITIALIZER;
    BSON_APPEND_INT32(&ping, "ping", 1);
    bson_t reply;
    bool ok = mongoc_client_command_simple(client, "admin", &ping, nullptr, &reply, &error);
    bson_destroy(&ping);
    bson_destroy(&reply);
    if (!ok) {
        mongoc_client_destroy(client);
        if (err)
            *err = error.message;
        return false;
    }

    bson_t cmd = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&cmd, "createIndexes", kColl);
    bson_t indexes;
    BSON_APPEND_ARRAY_BEGIN(&cmd, "indexes", &indexes);
    bson_t idx0;
    BSON_APPEND_DOCUMENT_BEGIN(&indexes, "0", &idx0);
    bson_t key;
    BSON_APPEND_DOCUMENT_BEGIN(&idx0, "key", &key);
    BSON_APPEND_INT32(&key, "player_id", 1);
    bson_append_document_end(&idx0, &key);
    BSON_APPEND_UTF8(&idx0, "name", "player_id_1");
    BSON_APPEND_BOOL(&idx0, "unique", true);
    bson_append_document_end(&indexes, &idx0);
    bson_append_array_end(&cmd, &indexes);
    bson_t idx_reply;
    mongoc_client_command_simple(client, db_name_.c_str(), &cmd, nullptr, &idx_reply, &error);
    bson_destroy(&cmd);
    bson_destroy(&idx_reply);

    client_ = client;
    return true;
}

PlayerState PlayerStore::Load(const std::string& player_id) {
    PlayerState st;
    st.player_id = player_id;
    if (!client_)
        return st;

    mongoc_collection_t* coll = mongoc_client_get_collection(
        static_cast<mongoc_client_t*>(client_), db_name_.c_str(), kColl);
    bson_t query = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&query, "player_id", player_id.c_str());
    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(coll, &query, nullptr, nullptr);
    bson_destroy(&query);

    const bson_t* doc = nullptr;
    if (mongoc_cursor_next(cursor, &doc) && doc) {
        bson_iter_t it;
        st.found = true;
        if (bson_iter_init_find(&it, doc, "x") && BSON_ITER_HOLDS_DOUBLE(&it))
            st.x = static_cast<float>(bson_iter_double(&it));
        if (bson_iter_init_find(&it, doc, "y") && BSON_ITER_HOLDS_DOUBLE(&it))
            st.y = static_cast<float>(bson_iter_double(&it));
        if (bson_iter_init_find(&it, doc, "z") && BSON_ITER_HOLDS_DOUBLE(&it))
            st.z = static_cast<float>(bson_iter_double(&it));
        if (bson_iter_init_find(&it, doc, "yaw") && BSON_ITER_HOLDS_DOUBLE(&it))
            st.yaw = static_cast<float>(bson_iter_double(&it));
        if (bson_iter_init_find(&it, doc, "wood") && BSON_ITER_HOLDS_INT32(&it))
            st.wood = bson_iter_int32(&it);
        if (bson_iter_init_find(&it, doc, "stone") && BSON_ITER_HOLDS_INT32(&it))
            st.stone = bson_iter_int32(&it);
    }

    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);
    return st;
}

PlayerStore::Result PlayerStore::Upsert(const PlayerState& state) {
    Result r;
    if (!client_) {
        r.error_msg = "db unavailable";
        return r;
    }

    mongoc_collection_t* coll = mongoc_client_get_collection(
        static_cast<mongoc_client_t*>(client_), db_name_.c_str(), kColl);

    bson_t filter = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&filter, "player_id", state.player_id.c_str());

    int64_t ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
    bson_t doc = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&doc, "player_id", state.player_id.c_str());
    BSON_APPEND_DOUBLE(&doc, "x", state.x);
    BSON_APPEND_DOUBLE(&doc, "y", state.y);
    BSON_APPEND_DOUBLE(&doc, "z", state.z);
    BSON_APPEND_DOUBLE(&doc, "yaw", state.yaw);
    BSON_APPEND_INT32(&doc, "wood", state.wood);
    BSON_APPEND_INT32(&doc, "stone", state.stone);
    BSON_APPEND_DATE_TIME(&doc, "updated_at", ms);

    bson_t opts = BSON_INITIALIZER;
    BSON_APPEND_BOOL(&opts, "upsert", true);

    bson_error_t error;
    bool ok = mongoc_collection_replace_one(coll, &filter, &doc, &opts, nullptr, &error);
    bson_destroy(&filter);
    bson_destroy(&doc);
    bson_destroy(&opts);
    mongoc_collection_destroy(coll);

    if (!ok) {
        r.error_msg = error.message ? error.message : "db unavailable";
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace game
