# MongoDB Account Auth (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add MongoDB-backed `Register` and username/password `Login` without blocking the epoll IO thread.

**Architecture:** `libmongoc` (apt) wrapped by `AccountStore`; `DbWorker` single thread; `EventLoop::RunInLoop` to marshal completions back to IO; `GameService` handles Register/Login then enters default world.

**Tech Stack:** C++20, libmongoc/libbson, libxcrypt (`crypt_r` bcrypt `$2b$`), CMake, existing tinrpc EventLoop

**Spec:** `docs/superpowers/specs/2026-08-05-mongo-account-auth-design.md`

**Note:** Spec mentions mongocxx; Ubuntu packages ship maintained **libmongoc**. Use libmongoc via thin C++ wrappers — same MongoDB wire protocol / local `mongod`.

---

## File map

| Path | Role |
|------|------|
| `proto/game.proto` | Register* + LoginReq username/password |
| `include/rpc/event_loop.h` + `src/event_loop.cpp` | `RunInLoop(std::function<void()>)` |
| `include/game/db_worker.h` + `src/game/db_worker.cpp` | Task queue + worker thread |
| `include/game/account_store.h` + `src/game/account_store.cpp` | Mongo accounts + bcrypt |
| `include/game/game_service.h` + `src/game/game_service.cpp` | Wire Register/Login |
| `CMakeLists.txt` | Link mongoc/bson/crypt |
| `tests/test_account_store.cpp` | Unit tests (needs mongod) |
| `tests/test_account_auth_e2e.cpp` | Register/Login e2e |
| `godot_client/scripts/net/proto_bridge.gd` | Encode new Login/Register |
| `README.md` | mongod + env vars |

Config: `TINRPC_MONGO_URI` (default `mongodb://127.0.0.1:27017`), `TINRPC_MONGO_DB` (default `tinrpc`; tests use `tinrpc_test`).

---

### Task 1: Proto breaking change

**Files:** `proto/game.proto`

- [ ] Replace `LoginReq.token` with `username` + `password`
- [ ] Add `RegisterReq` / `RegisterRes` before Login section
- [ ] Rebuild `rpc_lib`; fix compile breaks in tests that set `token` (update to username/password or temporary stubs)
- [ ] Commit: `feat(proto): register and username/password login messages`

---

### Task 2: EventLoop::RunInLoop

**Files:** `include/rpc/event_loop.h`, `src/event_loop.cpp`, optionally small unit test

- [ ] Add mutex-protected `std::vector<std::function<void()>>` pending queue
- [ ] `void RunInLoop(std::function<void()> fn)` — push + `eventfd_write(wakeup_fd_, 1)`
- [ ] On wakeup in `Run()`, after reading eventfd, swap-and-run all pending callbacks on IO thread
- [ ] Commit: `feat(rpc): EventLoop::RunInLoop for cross-thread callbacks`

---

### Task 3: DbWorker

**Files:** `include/game/db_worker.h`, `src/game/db_worker.cpp`

```cpp
class DbWorker {
public:
  using Task = std::function<void()>;
  explicit DbWorker(rpc::EventLoop* loop);
  ~DbWorker(); // stop + join
  void Start();
  void Stop();
  void Post(Task task); // runs on worker thread
  void PostToLoop(std::function<void()> fn); // loop->RunInLoop
private:
  rpc::EventLoop* loop_;
  std::thread thr_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Task> q_;
  bool stop_ = false;
};
```

- [ ] Implement Start/Stop/Post
- [ ] Commit: `feat: add DbWorker background task queue`

---

### Task 4: AccountStore (TDD against local mongod)

**Prereq:** `mongod` listening on 27017; packages `libmongoc-dev libbson-dev`.

- [ ] Implement `AccountStore` with CreateAccount / VerifyCredentials
- [ ] username unique index on init
- [ ] password: `crypt_gensalt("$2b$", ...)` + `crypt_r`; store full crypt string in `password_hash`
- [ ] player_id = username (Phase 1)
- [ ] Tests in `tests/test_account_store.cpp` using db `tinrpc_test`, drop collection in SetUp
- [ ] Commit: `feat: AccountStore MongoDB register/verify with bcrypt`

---

### Task 5: Wire GameService

- [ ] Own `DbWorker` + `AccountStore` (URI from env)
- [ ] `Register`: validate → DbWorker → RegisterRes (no world enter)
- [ ] `Login`: DbWorker verify → on success Leave old / RegisterPlayerConn / World.Enter / LoginRes
- [ ] Hold `Connection*` safely across async (fd + weak check: if conn still in map / fd matches)
- [ ] Update `test_world_e2e` / room tests Login helpers to username/password (may need test accounts seeded OR allow test mode — prefer seed via AccountStore in test setup)
- [ ] Commit: `feat: async Register/Login via AccountStore and DbWorker`

---

### Task 6: Godot proto_bridge + session

- [ ] Update Login encode/decode; add Register
- [ ] NetSession: register then login (or UI fields)
- [ ] Commit: `feat(godot): register and password login for account auth`

---

### Task 7: Docs + verify

- [ ] README: install mongod, libmongoc-dev, env vars, Register/Login
- [ ] Push branch; open/update PR

---

## Risk notes

- Existing e2e tests that `Login` with token must be migrated or they fail to compile.
- Do not commit unrelated dirty Godot assets (`.godot/`, `addons/`) unless required for Task 6.
- Connection lifetime across async Login: cancel if fd disconnected before callback.
