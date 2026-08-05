#include "game/game_service.h"
#include "game.pb.h"
#include "rpc/protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition)                                                                         \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            throw std::runtime_error("CHECK failed: " #condition);                              \
        }                                                                                        \
    } while (0)

namespace {

using Clock = std::chrono::steady_clock;

uint16_t FindFreePort() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);

    socklen_t length = sizeof(address);
    CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    const uint16_t port = ntohs(address.sin_port);
    close(fd);
    return port;
}

class SimpleClient {
public:
    ~SimpleClient() {
        Close();
    }

    void Connect(uint16_t port) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fd_ >= 0);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
        CHECK(connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    }

    void Close() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    void SendRequest(uint32_t request_id, const std::string& method,
                     const std::vector<uint8_t>& body) {
        const auto raw =
            rpc::ProtocolFrame::Encode(request_id, rpc::MessageType::Request, method, body);
        size_t offset = 0;
        while (offset < raw.size()) {
            const ssize_t sent =
                send(fd_, raw.data() + offset, raw.size() - offset, MSG_NOSIGNAL);
            CHECK(sent > 0);
            offset += static_cast<size_t>(sent);
        }
    }

    rpc::Frame ReceiveFrame(std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        auto header = ReceiveExact(13, deadline);
        const uint32_t total_length = (static_cast<uint32_t>(header[2]) << 24) |
                                      (static_cast<uint32_t>(header[3]) << 16) |
                                      (static_cast<uint32_t>(header[4]) << 8) |
                                      static_cast<uint32_t>(header[5]);
        CHECK(total_length >= 13 && total_length <= rpc::kMaxFrameSize);

        if (total_length > header.size()) {
            auto rest = ReceiveExact(total_length - header.size(), deadline);
            header.insert(header.end(), rest.begin(), rest.end());
        }
        auto frame = rpc::ProtocolFrame::Decode(header);
        CHECK(frame.has_value());
        return *frame;
    }

private:
    std::vector<uint8_t> ReceiveExact(size_t count, Clock::time_point deadline) {
        std::vector<uint8_t> data(count);
        size_t offset = 0;
        while (offset < count) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            CHECK(remaining.count() > 0);

            pollfd poll_fd{fd_, POLLIN, 0};
            CHECK(poll(&poll_fd, 1, static_cast<int>(remaining.count())) > 0);
            const ssize_t received = recv(fd_, data.data() + offset, count - offset, 0);
            CHECK(received > 0);
            offset += static_cast<size_t>(received);
        }
        return data;
    }

    int fd_ = -1;
};

std::vector<uint8_t> Serialize(const google::protobuf::MessageLite& message) {
    std::string buffer;
    CHECK(message.SerializeToString(&buffer));
    return {buffer.begin(), buffer.end()};
}

bool StateContains(const game::WorldStateNtf& state, const std::string& player_id) {
    for (const auto& player : state.players()) {
        if (player.player_id() == player_id) {
            return true;
        }
    }
    return false;
}

void LoginAndExpectSelfState(SimpleClient& client, uint32_t request_id, const std::string& token,
                             int expected_players) {
    // Phase 1: username/password — register then login (password fixed for tests)
    const std::string password = "testpass123";
    {
        game::RegisterReq reg;
        reg.set_username(token);
        reg.set_password(password);
        client.SendRequest(request_id, "Register", Serialize(reg));
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        bool got = false;
        while (!got) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            CHECK(remaining.count() > 0);
            const rpc::Frame frame = client.ReceiveFrame(remaining);
            if (frame.request_id == request_id && frame.method_name == "Register") {
                game::RegisterRes response;
                CHECK(response.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size())));
                // ok if success or username taken (re-run tests)
                got = true;
            }
        }
    }

    game::LoginReq request;
    request.set_username(token);
    request.set_password(password);
    client.SendRequest(request_id + 1000, "Login", Serialize(request));

    bool received_login = false;
    bool received_state = false;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!received_login || !received_state) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        CHECK(remaining.count() > 0);
        const rpc::Frame frame = client.ReceiveFrame(remaining);

        if (frame.request_id == request_id + 1000 && frame.method_name == "Login") {
            CHECK(frame.msg_type == rpc::MessageType::Response);
            game::LoginRes response;
            CHECK(response.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size())));
            CHECK(response.success());
            CHECK(response.player_info().player_id() == token);
            received_login = true;
        }
        if (frame.method_name == "WorldStateNtf") {
            game::WorldStateNtf state;
            CHECK(state.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size())));
            CHECK(StateContains(state, token));
            CHECK(state.players_size() == expected_players);
            received_state = true;
        }
    }
}

class RunningGameService {
public:
    RunningGameService() : port_(FindFreePort()), thread_([this] { service_.Run(port_); }) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~RunningGameService() {
        service_.Stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    uint16_t port() const {
        return port_;
    }

private:
    game::GameService service_;
    uint16_t port_;
    std::thread thread_;
};

void TestWorldLoginMoveLeave() {
    RunningGameService server;
    SimpleClient client_a;
    SimpleClient client_b;
    client_a.Connect(server.port());

    LoginAndExpectSelfState(client_a, 1, "player_a", 1);

    client_b.Connect(server.port());
    LoginAndExpectSelfState(client_b, 2, "player_b", 2);

    bool received_enter = false;
    const auto enter_deadline = Clock::now() + std::chrono::seconds(2);
    while (!received_enter) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(enter_deadline - Clock::now());
        CHECK(remaining.count() > 0);
        const rpc::Frame frame = client_a.ReceiveFrame(remaining);
        if (frame.method_name == "PlayerEnterNtf") {
            game::PlayerTransform enter;
            CHECK(enter.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size())));
            CHECK(enter.player_id() == "player_b");
            received_enter = true;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    game::MoveReq move;
    move.mutable_position()->set_x(1.0f);
    move.set_yaw(90.0f);
    client_a.SendRequest(3, "Move", Serialize(move));

    const rpc::Frame move_frame = client_a.ReceiveFrame(std::chrono::seconds(2));
    CHECK(move_frame.request_id == 3);
    CHECK(move_frame.msg_type == rpc::MessageType::Response);
    CHECK(move_frame.method_name == "Move");
    game::MoveRes move_response;
    CHECK(move_response.ParseFromArray(move_frame.body.data(),
                                       static_cast<int>(move_frame.body.size())));
    CHECK(move_response.success());
    CHECK(move_response.corrected_position().x() == 1.0f);

    bool received_move_state = false;
    const auto move_deadline = Clock::now() + std::chrono::seconds(2);
    while (!received_move_state) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(move_deadline - Clock::now());
        CHECK(remaining.count() > 0);
        const rpc::Frame frame = client_b.ReceiveFrame(remaining);
        if (frame.method_name == "WorldStateNtf") {
            game::WorldStateNtf state;
            CHECK(state.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size())));
            CHECK(state.players_size() == 1);
            CHECK(StateContains(state, "player_a"));
            received_move_state = true;
        }
    }

    client_a.Close();

    bool received_leave = false;
    const auto leave_deadline = Clock::now() + std::chrono::seconds(2);
    while (!received_leave) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(leave_deadline - Clock::now());
        CHECK(remaining.count() > 0);
        const rpc::Frame frame = client_b.ReceiveFrame(remaining);
        if (frame.method_name == "PlayerLeaveNtf") {
            game::WorldPlayerLeaveNtf leave;
            CHECK(leave.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size())));
            CHECK(leave.player_id() == "player_a");
            received_leave = true;
        }
    }
}

} // namespace

int main() {
    try {
        TestWorldLoginMoveLeave();
        printf("test_world_e2e: PASS\n");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "test_world_e2e: FAIL: %s\n", error.what());
        return 1;
    }
}
