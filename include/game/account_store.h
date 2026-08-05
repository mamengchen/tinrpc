#pragma once

#include <string>

namespace game {

/**
 * @brief MongoDB accounts 集合：注册 / 校验密码
 *
 * 线程模型：仅在 DbWorker 线程调用（mongoc 客户端非跨线程共享）。
 */
class AccountStore {
public:
    struct Result {
        bool ok = false;
        std::string error_msg;
        std::string player_id;
    };

    AccountStore(std::string uri, std::string db_name);
    ~AccountStore();

    AccountStore(const AccountStore&) = delete;
    AccountStore& operator=(const AccountStore&) = delete;

    /// 连接 Mongo 并为 username 建唯一索引；失败返回 false
    bool Init(std::string* err = nullptr);

    Result CreateAccount(const std::string& username, const std::string& password);
    Result VerifyCredentials(const std::string& username, const std::string& password);

    static bool ValidUsername(const std::string& username);
    static bool ValidPassword(const std::string& password);

private:
    std::string HashPassword(const std::string& password) const;
    bool VerifyPassword(const std::string& password, const std::string& password_hash) const;

    std::string uri_;
    std::string db_name_;
    void* client_ = nullptr; // mongoc_client_t*
};

} // namespace game
