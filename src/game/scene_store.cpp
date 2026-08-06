#include "game/scene_store.h"
#include "game/mongo_runtime.h"

#include <bson/bson.h>
#include <mongoc/mongoc.h>

#include <cstring>
#include <ctime>

namespace game {
namespace {

constexpr const char* kColl = "scenes";

} // namespace

SceneStore::SceneStore(std::string uri, std::string db_name)
    : uri_(std::move(uri)), db_name_(std::move(db_name)) {
    EnsureMongocInit();
}

SceneStore::~SceneStore() {
    if (client_) {
        mongoc_client_destroy(static_cast<mongoc_client_t*>(client_));
        client_ = nullptr;
    }
}

bool SceneStore::Init(std::string* err) {
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
    BSON_APPEND_INT32(&key, "scene_id", 1);
    bson_append_document_end(&idx0, &key);
    BSON_APPEND_UTF8(&idx0, "name", "scene_id_1");
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

SceneSnapshot SceneStore::Load(const std::string& scene_id) {
    SceneSnapshot snap;
    snap.scene_id = scene_id;
    if (!client_)
        return snap;

    mongoc_collection_t* coll = mongoc_client_get_collection(
        static_cast<mongoc_client_t*>(client_), db_name_.c_str(), kColl);
    bson_t query = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&query, "scene_id", scene_id.c_str());
    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(coll, &query, nullptr, nullptr);
    bson_destroy(&query);

    const bson_t* doc = nullptr;
    if (!mongoc_cursor_next(cursor, &doc) || !doc) {
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(coll);
        return snap;
    }

    snap.found = true;
    bson_iter_t it;
    if (bson_iter_init_find(&it, doc, "next_building_id") && BSON_ITER_HOLDS_INT64(&it))
        snap.next_building_id = static_cast<uint64_t>(bson_iter_int64(&it));
    else if (bson_iter_init_find(&it, doc, "next_building_id") && BSON_ITER_HOLDS_INT32(&it))
        snap.next_building_id = static_cast<uint64_t>(bson_iter_int32(&it));

    if (bson_iter_init_find(&it, doc, "resources") && BSON_ITER_HOLDS_ARRAY(&it)) {
        bson_iter_t arr;
        bson_iter_recurse(&it, &arr);
        while (bson_iter_next(&arr)) {
            if (!BSON_ITER_HOLDS_DOCUMENT(&arr))
                continue;
            bson_iter_t r;
            bson_iter_recurse(&arr, &r);
            SceneResource res;
            while (bson_iter_next(&r)) {
                const char* key = bson_iter_key(&r);
                if (strcmp(key, "id") == 0 && BSON_ITER_HOLDS_UTF8(&r))
                    res.id = bson_iter_utf8(&r, nullptr);
                else if (strcmp(key, "type") == 0 && BSON_ITER_HOLDS_INT32(&r))
                    res.type = bson_iter_int32(&r);
                else if (strcmp(key, "x") == 0 && BSON_ITER_HOLDS_DOUBLE(&r))
                    res.x = static_cast<float>(bson_iter_double(&r));
                else if (strcmp(key, "y") == 0 && BSON_ITER_HOLDS_DOUBLE(&r))
                    res.y = static_cast<float>(bson_iter_double(&r));
                else if (strcmp(key, "z") == 0 && BSON_ITER_HOLDS_DOUBLE(&r))
                    res.z = static_cast<float>(bson_iter_double(&r));
                else if (strcmp(key, "remaining") == 0 && BSON_ITER_HOLDS_INT32(&r))
                    res.remaining = bson_iter_int32(&r);
            }
            if (!res.id.empty())
                snap.resources.push_back(res);
        }
    }

    if (bson_iter_init_find(&it, doc, "buildings") && BSON_ITER_HOLDS_ARRAY(&it)) {
        bson_iter_t arr;
        bson_iter_recurse(&it, &arr);
        while (bson_iter_next(&arr)) {
            if (!BSON_ITER_HOLDS_DOCUMENT(&arr))
                continue;
            bson_iter_t b;
            bson_iter_recurse(&arr, &b);
            SceneBuilding building;
            while (bson_iter_next(&b)) {
                const char* key = bson_iter_key(&b);
                if (strcmp(key, "id") == 0 && BSON_ITER_HOLDS_UTF8(&b))
                    building.id = bson_iter_utf8(&b, nullptr);
                else if (strcmp(key, "owner_id") == 0 && BSON_ITER_HOLDS_UTF8(&b))
                    building.owner_id = bson_iter_utf8(&b, nullptr);
                else if (strcmp(key, "type") == 0 && BSON_ITER_HOLDS_INT32(&b))
                    building.type = bson_iter_int32(&b);
                else if (strcmp(key, "x") == 0 && BSON_ITER_HOLDS_DOUBLE(&b))
                    building.x = static_cast<float>(bson_iter_double(&b));
                else if (strcmp(key, "y") == 0 && BSON_ITER_HOLDS_DOUBLE(&b))
                    building.y = static_cast<float>(bson_iter_double(&b));
                else if (strcmp(key, "z") == 0 && BSON_ITER_HOLDS_DOUBLE(&b))
                    building.z = static_cast<float>(bson_iter_double(&b));
                else if (strcmp(key, "yaw") == 0 && BSON_ITER_HOLDS_DOUBLE(&b))
                    building.yaw = static_cast<float>(bson_iter_double(&b));
            }
            if (!building.id.empty())
                snap.buildings.push_back(building);
        }
    }

    if (bson_iter_init_find(&it, doc, "voxel_edits") && BSON_ITER_HOLDS_ARRAY(&it)) {
        bson_iter_t arr; bson_iter_recurse(&it, &arr);
        while (bson_iter_next(&arr)) {
            if (!BSON_ITER_HOLDS_DOCUMENT(&arr)) continue;
            bson_iter_t v; bson_iter_recurse(&arr, &v); SceneVoxelEdit edit;
            while (bson_iter_next(&v)) {
                const char* key = bson_iter_key(&v);
                if (strcmp(key, "room_id") == 0 && BSON_ITER_HOLDS_UTF8(&v)) edit.room_id = bson_iter_utf8(&v, nullptr);
                else if (strcmp(key, "x") == 0 && BSON_ITER_HOLDS_INT32(&v)) edit.x = bson_iter_int32(&v);
                else if (strcmp(key, "y") == 0 && BSON_ITER_HOLDS_INT32(&v)) edit.y = bson_iter_int32(&v);
                else if (strcmp(key, "z") == 0 && BSON_ITER_HOLDS_INT32(&v)) edit.z = bson_iter_int32(&v);
                else if (strcmp(key, "action") == 0 && BSON_ITER_HOLDS_INT32(&v)) edit.action = bson_iter_int32(&v);
                else if (strcmp(key, "block_type") == 0 && BSON_ITER_HOLDS_INT32(&v)) edit.block_type = bson_iter_int32(&v);
            }
            if (!edit.room_id.empty()) snap.voxel_edits.push_back(edit);
        }
    }

    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);
    return snap;
}

SceneStore::Result SceneStore::Upsert(const SceneSnapshot& snap) {
    Result r;
    if (!client_) {
        r.error_msg = "db unavailable";
        return r;
    }

    mongoc_collection_t* coll = mongoc_client_get_collection(
        static_cast<mongoc_client_t*>(client_), db_name_.c_str(), kColl);

    bson_t filter = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&filter, "scene_id", snap.scene_id.c_str());

    int64_t ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
    bson_t doc = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&doc, "scene_id", snap.scene_id.c_str());
    BSON_APPEND_INT64(&doc, "next_building_id", static_cast<int64_t>(snap.next_building_id));
    BSON_APPEND_DATE_TIME(&doc, "updated_at", ms);

    bson_t resources;
    BSON_APPEND_ARRAY_BEGIN(&doc, "resources", &resources);
    for (size_t i = 0; i < snap.resources.size(); ++i) {
        const auto& res = snap.resources[i];
        bson_t item;
        const std::string idx = std::to_string(i);
        BSON_APPEND_DOCUMENT_BEGIN(&resources, idx.c_str(), &item);
        BSON_APPEND_UTF8(&item, "id", res.id.c_str());
        BSON_APPEND_INT32(&item, "type", res.type);
        BSON_APPEND_DOUBLE(&item, "x", res.x);
        BSON_APPEND_DOUBLE(&item, "y", res.y);
        BSON_APPEND_DOUBLE(&item, "z", res.z);
        BSON_APPEND_INT32(&item, "remaining", res.remaining);
        bson_append_document_end(&resources, &item);
    }
    bson_append_array_end(&doc, &resources);

    bson_t buildings;
    BSON_APPEND_ARRAY_BEGIN(&doc, "buildings", &buildings);
    for (size_t i = 0; i < snap.buildings.size(); ++i) {
        const auto& b = snap.buildings[i];
        bson_t item;
        const std::string idx = std::to_string(i);
        BSON_APPEND_DOCUMENT_BEGIN(&buildings, idx.c_str(), &item);
        BSON_APPEND_UTF8(&item, "id", b.id.c_str());
        BSON_APPEND_UTF8(&item, "owner_id", b.owner_id.c_str());
        BSON_APPEND_INT32(&item, "type", b.type);
        BSON_APPEND_DOUBLE(&item, "x", b.x);
        BSON_APPEND_DOUBLE(&item, "y", b.y);
        BSON_APPEND_DOUBLE(&item, "z", b.z);
        BSON_APPEND_DOUBLE(&item, "yaw", b.yaw);
        bson_append_document_end(&buildings, &item);
    }
    bson_append_array_end(&doc, &buildings);

    bson_t voxel_edits;
    BSON_APPEND_ARRAY_BEGIN(&doc, "voxel_edits", &voxel_edits);
    for (size_t i = 0; i < snap.voxel_edits.size(); ++i) {
        const auto& v = snap.voxel_edits[i]; bson_t item; const std::string idx = std::to_string(i);
        BSON_APPEND_DOCUMENT_BEGIN(&voxel_edits, idx.c_str(), &item);
        BSON_APPEND_UTF8(&item, "room_id", v.room_id.c_str());
        BSON_APPEND_INT32(&item, "x", v.x); BSON_APPEND_INT32(&item, "y", v.y); BSON_APPEND_INT32(&item, "z", v.z);
        BSON_APPEND_INT32(&item, "action", v.action); BSON_APPEND_INT32(&item, "block_type", v.block_type);
        bson_append_document_end(&voxel_edits, &item);
    }
    bson_append_array_end(&doc, &voxel_edits);

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
