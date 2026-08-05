#include "game/world_service.h"
#include "game.pb.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>

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
    assert(!sent.empty());
    assert(sent.back().first == "a");
    assert(sent.back().second == "WorldStateNtf");
    sent.clear();

    world.Enter("b", "Bob", 1100);
    // b 收到 WorldStateNtf；a 收到 PlayerEnterNtf
    bool b_snap = false, a_enter = false;
    for (auto& p : sent) {
        if (p.first == "b" && p.second == "WorldStateNtf") b_snap = true;
        if (p.first == "a" && p.second == "PlayerEnterNtf") a_enter = true;
    }
    assert(b_snap && a_enter);
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
    assert(!res.success);
    assert(res.corrected_x == 0.f);
    // 不应向 b 广播
    for (auto& p : sent) {
        assert(!(p.first == "b" && p.second == "WorldStateNtf"));
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
    assert(res.success);
    bool b_got = false;
    for (auto& p : sent) {
        if (p.first == "b" && p.second == "WorldStateNtf") b_got = true;
    }
    assert(b_got);

    sent.clear();
    world.Leave("a");
    bool leave = false;
    for (auto& p : sent) {
        if (p.first == "b" && p.second == "PlayerLeaveNtf") leave = true;
    }
    assert(leave);
    assert(!world.HasPlayer("a"));
}

int main() {
    printf("=== test_world_service ===\n");
    RunTest("TestEnterSnapshotAndNotify", TestEnterSnapshotAndNotify);
    RunTest("TestMoveSpeedReject", TestMoveSpeedReject);
    RunTest("TestMoveBroadcastAndLeave", TestMoveBroadcastAndLeave);
    printf("Result: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
