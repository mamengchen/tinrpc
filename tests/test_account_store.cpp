#include "game/account_store.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond))                                                                               \
            throw std::runtime_error(#cond);                                                       \
    } while (0)

static int g_passed = 0, g_failed = 0;
void RunTest(const char* name, void (*fn)()) {
    printf("  %-55s ... ", name);
    try {
        fn();
        printf("[PASS]\n");
        g_passed++;
    } catch (const std::exception& e) {
        printf("[FAIL] %s\n", e.what());
        g_failed++;
    }
}

void TestRegisterAndLogin() {
    const char* uri = std::getenv("TINRPC_MONGO_URI");
    if (!uri || !*uri)
        uri = "mongodb://127.0.0.1:27017";
    game::AccountStore store(uri, "tinrpc_test");
    std::string err;
    CHECK(store.Init(&err));

    const std::string user = "ut_user_a";
    const std::string pass = "secret12";

    auto r1 = store.CreateAccount(user, pass);
    if (!r1.ok && r1.error_msg == "username taken") {
        // already there from previous run — verify login works
    } else {
        CHECK(r1.ok);
        CHECK(r1.player_id == user);
    }

    auto dup = store.CreateAccount(user, pass);
    CHECK(!dup.ok);
    CHECK(dup.error_msg == "username taken");

    auto bad = store.VerifyCredentials(user, "wrongpass");
    CHECK(!bad.ok);

    auto ok = store.VerifyCredentials(user, pass);
    CHECK(ok.ok);
    CHECK(ok.player_id == user);
}

void TestValidation() {
    CHECK(!game::AccountStore::ValidUsername("ab"));
    CHECK(!game::AccountStore::ValidUsername("bad-name"));
    CHECK(game::AccountStore::ValidUsername("good_1"));
    CHECK(!game::AccountStore::ValidPassword("123"));
    CHECK(game::AccountStore::ValidPassword("123456"));
}

int main() {
    printf("=== test_account_store ===\n");
    RunTest("TestValidation", TestValidation);
    RunTest("TestRegisterAndLogin", TestRegisterAndLogin);
    printf("Result: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
