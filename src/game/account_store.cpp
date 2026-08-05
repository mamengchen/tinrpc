#include "game/account_store.h"
#include "game/mongo_runtime.h"

#include <bson/bson.h>
#include <mongoc/mongoc.h>

#include <cctype>
#include <crypt.h>
#include <ctime>
#include <cstring>

namespace game {
namespace {

constexpr const char* kColl = "accounts";

} // namespace

AccountStore::AccountStore(std::string uri, std::string db_name)
    : uri_(std::move(uri)), db_name_(std::move(db_name)) {
    EnsureMongocInit();
}

AccountStore::~AccountStore() {
    if (client_) {
        mongoc_client_destroy(static_cast<mongoc_client_t*>(client_));
        client_ = nullptr;
    }
}

bool AccountStore::ValidUsername(const std::string& username) {
    if (username.size() < 3 || username.size() > 32)
        return false;
    for (unsigned char c : username) {
        if (!(std::isalnum(c) || c == '_'))
            return false;
    }
    return true;
}

bool AccountStore::ValidPassword(const std::string& password) {
    return password.size() >= 6 && password.size() <= 64;
}

bool AccountStore::Init(std::string* err) {
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

    // createIndexes: unique username
    bson_t cmd = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&cmd, "createIndexes", kColl);
    bson_t indexes;
    BSON_APPEND_ARRAY_BEGIN(&cmd, "indexes", &indexes);
    bson_t idx0;
    BSON_APPEND_DOCUMENT_BEGIN(&indexes, "0", &idx0);
    bson_t key;
    BSON_APPEND_DOCUMENT_BEGIN(&idx0, "key", &key);
    BSON_APPEND_INT32(&key, "username", 1);
    bson_append_document_end(&idx0, &key);
    BSON_APPEND_UTF8(&idx0, "name", "username_1");
    BSON_APPEND_BOOL(&idx0, "unique", true);
    bson_append_document_end(&indexes, &idx0);
    bson_append_array_end(&cmd, &indexes);

    bson_t idx_reply;
    ok = mongoc_client_command_simple(client, db_name_.c_str(), &cmd, nullptr, &idx_reply, &error);
    bson_destroy(&cmd);
    bson_destroy(&idx_reply);
    if (!ok && err) {
        // Index may already exist — still usable if ping worked
        (void)error;
    }

    client_ = client;
    return true;
}

std::string AccountStore::HashPassword(const std::string& password) const {
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    if (!crypt_gensalt_rn("$2b$", 0, nullptr, 0, salt, sizeof(salt))) {
        return {};
    }
    struct crypt_data data {};
    data.initialized = 0;
    char* hash = crypt_r(password.c_str(), salt, &data);
    if (!hash)
        return {};
    return std::string(hash);
}

bool AccountStore::VerifyPassword(const std::string& password,
                                  const std::string& password_hash) const {
    struct crypt_data data {};
    data.initialized = 0;
    char* out = crypt_r(password.c_str(), password_hash.c_str(), &data);
    return out != nullptr && password_hash == out;
}

AccountStore::Result AccountStore::CreateAccount(const std::string& username,
                                                 const std::string& password) {
    Result r;
    if (!client_) {
        r.error_msg = "db unavailable";
        return r;
    }
    if (!ValidUsername(username) || !ValidPassword(password)) {
        r.error_msg = "invalid username/password";
        return r;
    }

    std::string hash = HashPassword(password);
    if (hash.empty()) {
        r.error_msg = "db unavailable";
        return r;
    }

    mongoc_collection_t* coll = mongoc_client_get_collection(
        static_cast<mongoc_client_t*>(client_), db_name_.c_str(), kColl);

    int64_t ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
    bson_t doc = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&doc, "username", username.c_str());
    BSON_APPEND_UTF8(&doc, "password_hash", hash.c_str());
    BSON_APPEND_UTF8(&doc, "player_id", username.c_str());
    BSON_APPEND_DATE_TIME(&doc, "created_at", ms);
    BSON_APPEND_DATE_TIME(&doc, "updated_at", ms);

    bson_error_t error;
    bool ok = mongoc_collection_insert_one(coll, &doc, nullptr, nullptr, &error);
    bson_destroy(&doc);
    mongoc_collection_destroy(coll);

    if (!ok) {
        if (error.code == 11000) {
            r.error_msg = "username taken";
        } else {
            r.error_msg = "db unavailable";
        }
        return r;
    }

    r.ok = true;
    r.player_id = username;
    return r;
}

AccountStore::Result AccountStore::VerifyCredentials(const std::string& username,
                                                     const std::string& password) {
    Result r;
    if (!client_) {
        r.error_msg = "db unavailable";
        return r;
    }
    if (!ValidUsername(username) || !ValidPassword(password)) {
        r.error_msg = "invalid credentials";
        return r;
    }

    mongoc_collection_t* coll = mongoc_client_get_collection(
        static_cast<mongoc_client_t*>(client_), db_name_.c_str(), kColl);

    bson_t query = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&query, "username", username.c_str());
    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(coll, &query, nullptr, nullptr);
    bson_destroy(&query);

    const bson_t* doc = nullptr;
    if (!mongoc_cursor_next(cursor, &doc) || !doc) {
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(coll);
        r.error_msg = "invalid credentials";
        return r;
    }

    bson_iter_t it;
    std::string stored_hash;
    std::string player_id = username;
    if (bson_iter_init_find(&it, doc, "password_hash") && BSON_ITER_HOLDS_UTF8(&it)) {
        stored_hash = bson_iter_utf8(&it, nullptr);
    }
    if (bson_iter_init_find(&it, doc, "player_id") && BSON_ITER_HOLDS_UTF8(&it)) {
        player_id = bson_iter_utf8(&it, nullptr);
    }

    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);

    if (stored_hash.empty() || !VerifyPassword(password, stored_hash)) {
        r.error_msg = "invalid credentials";
        return r;
    }

    r.ok = true;
    r.player_id = player_id;
    return r;
}

} // namespace game
