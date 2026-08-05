#include "game/player_store.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
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

void TestUpsertAndLoad() {
    const char* uri = std::getenv("TINRPC_MONGO_URI");
    if (!uri || !*uri)
        uri = "mongodb://127.0.0.1:27017";
    game::PlayerStore store(uri, "tinrpc_test");
    std::string err;
    CHECK(store.Init(&err));

    game::PlayerState st;
    st.player_id = "persist_ut_1";
    st.x = 12.5f;
    st.y = 0.f;
    st.z = -3.f;
    st.yaw = 90.f;
    st.wood = 9;
    st.stone = 4;
    auto up = store.Upsert(st);
    CHECK(up.ok);

    auto loaded = store.Load("persist_ut_1");
    CHECK(loaded.found);
    CHECK(std::fabs(loaded.x - 12.5f) < 1e-3f);
    CHECK(std::fabs(loaded.z + 3.f) < 1e-3f);
    CHECK(std::fabs(loaded.yaw - 90.f) < 1e-3f);
    CHECK(loaded.wood == 9);
    CHECK(loaded.stone == 4);

    auto missing = store.Load("no_such_player_zzz");
    CHECK(!missing.found);
}

int main() {
    printf("=== test_player_store ===\n");
    RunTest("TestUpsertAndLoad", TestUpsertAndLoad);
    printf("Result: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
