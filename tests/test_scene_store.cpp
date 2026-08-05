#include "game/scene_store.h"
#include "game/world_service.h"

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

void TestSceneUpsertLoad() {
    const char* uri = std::getenv("TINRPC_MONGO_URI");
    if (!uri || !*uri)
        uri = "mongodb://127.0.0.1:27017";
    game::SceneStore store(uri, "tinrpc_test");
    std::string err;
    CHECK(store.Init(&err));

    game::WorldService world([](const std::string&, const std::string&, const std::vector<uint8_t>&) {});
    world.SeedDefaultScene();
    auto snap = world.ExportScene("default");
    snap.resources[0].remaining = 2;
    CHECK(store.Upsert(snap).ok);

    auto loaded = store.Load("default");
    CHECK(loaded.found);
    CHECK(!loaded.resources.empty());
    bool found = false;
    for (const auto& r : loaded.resources) {
        if (r.id == snap.resources[0].id) {
            CHECK(r.remaining == 2);
            found = true;
        }
    }
    CHECK(found);

    game::WorldService world2([](const std::string&, const std::string&, const std::vector<uint8_t>&) {});
    world2.LoadScene(loaded);
    auto again = world2.ExportScene("default");
    CHECK(again.resources.size() == loaded.resources.size());
}

int main() {
    printf("=== test_scene_store ===\n");
    RunTest("TestSceneUpsertLoad", TestSceneUpsertLoad);
    printf("Result: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
