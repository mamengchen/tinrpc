#include "game/world_service.h"
#include "game.pb.h"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define CHECK(cond) do { if (!(cond)) throw std::runtime_error(#cond); } while (0)

static int g_passed = 0, g_failed = 0;
void RunTest(const char* name, void (*fn)()) {
    printf("  %-55s ... ", name);
    try { fn(); printf("[PASS]\n"); g_passed++; }
    catch (...) { printf("[FAIL]\n"); g_failed++; }
}

void TestEnterSnapshotAndNotify() {
    std::vector<std::pair<std::string, std::string>> sent; // player_id, method
    game::WorldService world([&](const std::string& to, const std::string& method,
                                 const std::vector<uint8_t>&) {
        sent.emplace_back(to, method);
    });

    world.Enter("a", "Alice", /*now_ms=*/1000);
    // a 应收到 WorldStateNtf（含自己）
    CHECK(!sent.empty());
    CHECK(sent.back().first == "a");
    CHECK(sent.back().second == "WorldStateNtf");
    sent.clear();

    world.Enter("b", "Bob", 1100);
    // b 收到 WorldStateNtf；a 收到 PlayerEnterNtf
    bool b_snap = false, a_enter = false;
    for (auto& p : sent) {
        if (p.first == "b" && p.second == "WorldStateNtf") b_snap = true;
        if (p.first == "a" && p.second == "PlayerEnterNtf") a_enter = true;
    }
    CHECK(b_snap && a_enter);
}

void TestMoveSpeedReject() {
    std::vector<std::pair<std::string, std::string>> sent;
    game::WorldService world([&](const std::string& to, const std::string& method,
                                 const std::vector<uint8_t>&) {
        sent.emplace_back(to, method);
    });
    world.Enter("a", "A", 1000);
    world.Enter("b", "B", 1000);
    sent.clear();

    // 1ms 内移动 100m → 超速
    auto res = world.TryMove("a", 100.f, 0.f, 0.f, 0.f, /*now_ms=*/1001);
    CHECK(!res.success);
    CHECK(res.corrected_x == 0.f);
    // 不应向 b 广播
    for (auto& p : sent) {
        CHECK(!(p.first == "b" && p.second == "WorldStateNtf"));
    }
}

void TestMoveBroadcastAndLeave() {
    std::vector<std::pair<std::string, std::string>> sent;
    game::WorldService world([&](const std::string& to, const std::string& method,
                                 const std::vector<uint8_t>&) {
        sent.emplace_back(to, method);
    });
    world.Enter("a", "A", 1000);
    world.Enter("b", "B", 1000);
    sent.clear();

    auto res = world.TryMove("a", 1.f, 0.f, 0.f, 90.f, /*now_ms=*/1200); // 0.2s * 10m/s ok
    CHECK(res.success);
    bool b_got = false;
    for (auto& p : sent) {
        if (p.first == "b" && p.second == "WorldStateNtf") b_got = true;
    }
    CHECK(b_got);

    sent.clear();
    world.Leave("a");
    bool leave = false;
    for (auto& p : sent) {
        if (p.first == "b" && p.second == "PlayerLeaveNtf") leave = true;
    }
    CHECK(leave);
    CHECK(!world.HasPlayer("a"));
}

void TestGatherAndBuildAuthoritativeState() {
    std::vector<std::pair<std::string, std::string>> sent;
    game::WorldService world([&](const std::string& to, const std::string& method,
                                 const std::vector<uint8_t>&) { sent.emplace_back(to, method); });
    world.SeedDefaultScene();
    world.Enter("a", "A", 1000);
    world.Enter("b", "B", 1000);
    CHECK(world.TryMove("a", -4.f, 0.f, -4.f, 0.f, 2000).success);

    auto gather = world.TryGather("a", "tree_1");
    CHECK(gather.success);
    CHECK(gather.remaining == 5);
    CHECK(gather.wood == 7);
    bool resource_broadcast = false;
    for (const auto& [to, method] : sent) {
        if (to == "b" && method == "ResourceChangedNtf") resource_broadcast = true;
    }
    CHECK(resource_broadcast);

    auto floor = world.TryPlaceBuilding("a", 1, -3.f, 0.f, -4.f, 0.f);
    CHECK(floor.success);
    CHECK(floor.wood == 5);
    CHECK(!floor.building_id.empty());
    auto occupied = world.TryPlaceBuilding("a", 1, -3.f, 0.f, -4.f, 0.f);
    CHECK(!occupied.success);
}

void TestGatherDistanceAndBuildCostReject() {
    game::WorldService world([](const std::string&, const std::string&,
                                const std::vector<uint8_t>&) {});
    world.SeedDefaultScene();
    world.Enter("a", "A", 1000);
    auto gather = world.TryGather("a", "tree_4");
    CHECK(!gather.success);
    auto first = world.TryPlaceBuilding("a", 3, 1.f, 0.f, 0.f, 0.f);
    CHECK(first.success);
    auto second = world.TryPlaceBuilding("a", 3, 2.f, 0.f, 0.f, 0.f);
    CHECK(!second.success);
}

void TestMoveBurstAtSameTimestampUsesBoundedAllowance() {
    game::WorldService world([](const std::string&, const std::string&,
                                const std::vector<uint8_t>&) {});
    world.Enter("a", "A", 1000);
    CHECK(world.TryMove("a", 0.2f, 0.f, 0.f, 0.f, 1001).success);
    CHECK(world.TryMove("a", 0.4f, 0.f, 0.f, 0.f, 1001).success);
    CHECK(world.TryMove("a", 0.6f, 0.f, 0.f, 0.f, 1001).success);
    CHECK(world.TryMove("a", 0.8f, 0.f, 0.f, 0.f, 1001).success);
    CHECK(world.TryMove("a", 1.0f, 0.f, 0.f, 0.f, 1001).success);
    auto rejected = world.TryMove("a", 1.3f, 0.f, 0.f, 0.f, 1001);
    CHECK(!rejected.success);
    CHECK(rejected.error_msg == "movement too fast");
}

void TestVoxelMaterialEconomyAndDuplicateGuard() {
    game::WorldService world([](const std::string&, const std::string&,
                                const std::vector<uint8_t>&) {});
    world.Enter("a", "A", 1000);
    std::string error;
    int wood = 0, stone = 0, dirt = 0, copper = 0;
    CHECK(world.ApplyVoxelEdit("a", 1, 0, 0, 1, 2, &error,
                               &wood, &stone, &dirt, &copper));
    CHECK(wood == 6 && stone == 3 && dirt == 13 && copper == 0);
    CHECK(!world.ApplyVoxelEdit("a", 1, 0, 0, 1, 2, &error,
                                &wood, &stone, &dirt, &copper));
    CHECK(error == "block already removed");
    CHECK(world.ApplyVoxelEdit("a", 1, 0, 0, 2, 4, &error,
                               &wood, &stone, &dirt, &copper));
    CHECK(wood == 5 && dirt == 13);
    CHECK(world.ApplyVoxelEdit("a", 1, 0, 0, 1, 4, &error,
                               &wood, &stone, &dirt, &copper));
    CHECK(wood == 6 && dirt == 13);
}

void TestCraftProgressionAndCosts() {
    game::WorldService world([](const std::string&, const std::string&,
                                const std::vector<uint8_t>&) {});
    world.EnterWithState("a", "A", 1000, 0, 0, 0, 0, 10, 10, 12, 10, 0);
    auto stone_first = world.TryCraft("a", 2);
    CHECK(!stone_first.success && stone_first.error_msg == "previous tool required");
    auto wood_pick = world.TryCraft("a", 1);
    CHECK(wood_pick.success && wood_pick.wood == 8 && wood_pick.tool_level == 1);
    auto stone_pick = world.TryCraft("a", 2);
    CHECK(stone_pick.success && stone_pick.wood == 6 && stone_pick.stone == 5);
    auto copper_pick = world.TryCraft("a", 3);
    CHECK(copper_pick.success && copper_pick.wood == 4 && copper_pick.copper == 5);
    CHECK(!world.TryCraft("a", 3).success);
}

int main() {
    printf("=== test_world_service ===\n");
    RunTest("TestEnterSnapshotAndNotify", TestEnterSnapshotAndNotify);
    RunTest("TestMoveSpeedReject", TestMoveSpeedReject);
    RunTest("TestMoveBroadcastAndLeave", TestMoveBroadcastAndLeave);
    RunTest("TestGatherAndBuildAuthoritativeState", TestGatherAndBuildAuthoritativeState);
    RunTest("TestGatherDistanceAndBuildCostReject", TestGatherDistanceAndBuildCostReject);
    RunTest("TestMoveBurstAtSameTimestampUsesBoundedAllowance",
            TestMoveBurstAtSameTimestampUsesBoundedAllowance);
    RunTest("TestVoxelMaterialEconomyAndDuplicateGuard",
            TestVoxelMaterialEconomyAndDuplicateGuard);
    RunTest("TestCraftProgressionAndCosts", TestCraftProgressionAndCosts);
    printf("Result: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
