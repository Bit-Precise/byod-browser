# P1-D · Guardian IPC & 会话钩子补丁 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 合入 3 组 Chromium 补丁：Browser↔Guardian IPC 握手/心跳骨架（补丁 #12）、Exam Session 生命周期状态机 + 熔断入口（补丁 #13）、blur/focus/resize/nav 事件桥接（补丁 #14）。覆盖 spec §3.1 补丁 #12、#13、#14 与 spec §5.1 IPC 信任根、§4.1.2 Kiosk 事件、§4.1.5 事件监控。

**Architecture:** 分三段：
- **§A · IPC**：新增 `chrome/browser/exam/ipc/` 目录，含 handshake handshake helper（平台特化的 fork + 密钥传递占位）、心跳状态机、消息帧编码 —— **真实的加密协议由 P2 交付；本 sub-plan 只做骨架 + 接口冻结**
- **§B · Session**：新增 `chrome/browser/exam/session/` 目录，含 `ExamSession` 单例、状态机（`kIdle` / `kActive` / `kBurning` / `kEnded`）、熔断入口 API
- **§C · Events**：新增 `chrome/browser/exam/events/` 目录，含窗口事件 forwarder（挂到 BrowserView delegate）、事件 struct、送入 Session queue 的路径

三段互相依赖（Events → Session → IPC，因为 Events 触发时可能需要经 IPC 上报 Guardian、Session 熔断时需要通过 IPC 通知 Guardian）—— 按 §A → §B → §C 顺序写。

**Tech Stack:** Chromium C++（`chrome/browser/exam/`）、`base::NoDestructor` 单例、`base::TimeTicks`（单调时钟）、bash smoke tests、平台特化的 fork API（Win: `CreateProcess`；mac: `posix_spawn` —— **本 sub-plan 仅提供接口，真实调用由 P2 里的 Guardian 实现方接入**）。

## Global Constraints

- Chromium 补丁总量 ≤ 3000 行（全 P1 累计）；本批预算 ≤ 900 行；P1-A + P1-B + P1-C + P1-D 累计目标 ≤ 2530 行
- Chromium 补丁只做"删/禁"和"钩子暴露"，**不写业务**（真正的握手加密、心跳 HMAC 计算、Guardian 端实现由 P2 交付；策略层的熔断决策由 P5 交付；事件规则匹配由 P5 交付）
- 每个补丁必须独立可 revert；即使全部 revert 掉，Chromium 也能编译并启动（default no-op 实现）
- 每个补丁的 commit message 前缀固定 `byod: `；spec 章节引用固定 `see spec §3.1 patch #N`
- 补丁文件命名遵循 P0 patch-workflow.md 的序号分段：`0600-0699` = IPC 与 Guardian；`0700-0799` = Session/Config；`0800-0899` = 事件采集钩子
- P1-D 依赖 P1-A + P1-B + P1-C 已合入（22 个 patch）
- **本 sub-plan 冻结的接口**：`ExamSession` API、`GuardianTransport` 接口、`ExamEventSink` 接口 —— P2/P5/P6 均依赖，签名一经冻结不可改（否则本 patch 要跟着回炉）

**架构原则声明**（本批容易违反，特别标注）：

- IPC 握手的**加密逻辑**（HMAC / 密钥派生 / attest 校验）**不在本 sub-plan**；本 sub-plan 只提供"传密钥的管道"和"心跳消息的收发"抽象
- Session 状态机的**决策规则**（什么事件触发熔断、什么阈值）**不在本 sub-plan**；本 sub-plan 只提供 `TriggerBurn(reason)` API 和状态转换代码
- Guardian 的实际启动（fork exec）**不在本 sub-plan**；本 sub-plan 只在 Chromium 侧准备好"接受 Guardian 连接"的通道；P2 会写真正的 fork/exec 代码到 Guardian helper

---

## File Structure

**本 sub-plan 交付：**

```
/root/byod-browser/
├── patches/
│   ├── 0600-add-guardian-transport-interface.patch      # 补丁 #12a (接口)
│   ├── 0601-add-heartbeat-state-machine.patch           # 补丁 #12b (心跳状态机)
│   ├── 0602-wire-guardian-launch-in-browser-main.patch  # 补丁 #12c (fork 挂钩)
│   ├── 0700-add-exam-session-singleton.patch            # 补丁 #13a (Session 单例)
│   ├── 0701-add-exam-session-state-machine.patch        # 补丁 #13b (状态机)
│   ├── 0702-wire-burn-signal-to-browser-close.patch     # 补丁 #13c (熔断→ AllowClose)
│   ├── 0800-add-exam-event-sink-interface.patch         # 补丁 #14a (Sink 接口)
│   ├── 0801-forward-window-blur-focus-resize.patch      # 补丁 #14b (窗口事件 hook)
│   └── 0802-forward-navigation-events.patch             # 补丁 #14c (导航事件 hook)
├── build/
│   └── smoke-test-p1d.sh                                # 本批专用行为验证脚本
└── docs/
    └── development/
        └── p1-d-acceptance.md                           # 本批验收清单
```

**Chromium 源码触及区域：**

- 新增 `chrome/browser/exam/BUILD.gn`
- 新增 `chrome/browser/exam/ipc/guardian_transport.h` + `.cc`（#12a）
- 新增 `chrome/browser/exam/ipc/heartbeat_state_machine.h` + `.cc`（#12b）
- 新增 `chrome/browser/exam/ipc/guardian_launcher.h` + `.cc`（#12c，平台桩）
- 修改 `chrome/browser/chrome_browser_main.cc`（#12c，PreCreateThreads 挂钩）
- 新增 `chrome/browser/exam/session/exam_session.h` + `.cc`（#13a #13b）
- 修改 `chrome/browser/ui/browser.cc`（#13c，接 SetByodAllowClose）
- 新增 `chrome/browser/exam/events/exam_event_sink.h` + `.cc`（#14a）
- 新增 `chrome/browser/exam/events/window_event_forwarder.h` + `.cc`（#14b）
- 修改 `chrome/browser/ui/views/frame/browser_view.cc`（#14b，挂 delegate）
- 修改 `chrome/browser/ui/browser.cc` / `chrome/browser/ui/browser_tab_contents.cc`（#14c，挂 navigation observer）
- `chrome/browser/BUILD.gn` 加 `//chrome/browser/exam` deps

---

# §A · IPC 骨架（补丁 #12）

## Task A1: 补丁 #12a — 定义 GuardianTransport 接口

**Files:**
- Create (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create (in Chromium src): `chrome/browser/exam/ipc/guardian_transport.h`
- Create (in Chromium src): `chrome/browser/exam/ipc/guardian_transport.cc`
- Modify (in Chromium src): `chrome/browser/BUILD.gn` — 加对新 exam target 的 dep
- Create: `/root/byod-browser/patches/0600-add-guardian-transport-interface.patch`
- Create: `/root/byod-browser/build/smoke-test-p1d.sh`

**Interfaces:**
- Consumes: P1-C 全部完成（22 个 patch 已合入）
- **Produces**（P2 消费）:
  ```cpp
  namespace chrome::exam {

  // Serialized frame sent between Browser and Guardian.
  struct GuardianFrame {
    uint64_t seq;                     // Monotonic per direction.
    uint32_t type;                    // Message type enum (see MessageType).
    std::vector<uint8_t> payload;     // Opaque bytes, HMAC verified by P2.
    std::vector<uint8_t> hmac;        // HMAC-SHA256 tag (empty in P1-D stub).
  };

  enum class MessageType : uint32_t {
    kHandshakeHello = 1,
    kHandshakeAck = 2,
    kHeartbeat = 10,
    kChallenge = 20,
    kChallengeResponse = 21,
    kEvent = 30,              // Guardian → Browser event report
    kBurnCommand = 40,        // Browser → Guardian: exam is burning
  };

  // Abstract transport. P1-D provides a NullTransport (no-op). P2 replaces
  // it with a real pipe/socketpair-backed transport once Guardian is running.
  class GuardianTransport {
   public:
    // Callback invoked on the browser UI thread when a frame arrives from
    // Guardian. Never called reentrantly with Send().
    using OnFrameCallback =
        base::RepeatingCallback<void(const GuardianFrame&)>;

    // Send a frame to Guardian. Non-blocking; returns false if transport is
    // dead (e.g. Guardian crashed).
    virtual bool Send(const GuardianFrame& frame) = 0;

    // Register the frame receive callback. Called at most once per
    // transport instance.
    virtual void SetOnFrameCallback(OnFrameCallback cb) = 0;

    // True if the transport is currently connected (Guardian alive & pipe OK).
    virtual bool IsConnected() const = 0;

    virtual ~GuardianTransport() = default;
  };

  // Global setter/getter. Default is a NullTransport (Send always returns
  // false, IsConnected() = false).
  void SetGuardianTransport(std::unique_ptr<GuardianTransport> transport);
  GuardianTransport* GetGuardianTransport();

  }  // namespace chrome::exam
  ```

- [ ] **Step 1: 起始状态自检**

Run:
```bash
cd /opt/chromium/src
git status
git describe --tags
git log --oneline | head -25
```
Expected: `working tree clean`；tag 一致；最近 22 条 commit 是 P1-A + P1-B + P1-C 的补丁（消息以 `byod:` 开头）。

若不干净：`echo y | bash /root/byod-browser/build/revert-patches.sh && bash /root/byod-browser/build/apply-patches.sh`

- [ ] **Step 2: 写 smoke-test-p1d.sh 骨架**

Write `/root/byod-browser/build/smoke-test-p1d.sh`:

```bash
#!/usr/bin/env bash
# Behavior smoke tests for P1-D patches (#12, #13, #14).
# Run after applying patches and rebuilding chrome.
set -euo pipefail

CHROME_BIN="${1:-/opt/chromium/src/out/linux/chrome}"
if [ ! -x "$CHROME_BIN" ]; then
  echo "[error] chrome not found: $CHROME_BIN"; exit 1
fi

CHROME_TMP="$(mktemp -d)"
trap 'rm -rf "$CHROME_TMP"' EXIT

run_chrome() {
  "$CHROME_BIN" \
    --user-data-dir="$CHROME_TMP/profile-$$-$RANDOM" \
    --no-first-run \
    --no-default-browser-check \
    --disable-gpu \
    --no-sandbox \
    "$@" 2>&1
}

pass() { echo "  ✓ $1"; }
fail() { echo "  ✗ $1"; FAILED=1; }
FAILED=0

# ---------- Patch #12a: transport interface header exists ----------
echo "[test] patch #12a: GuardianTransport symbols compiled in"

if [ -f /opt/chromium/src/chrome/browser/exam/ipc/guardian_transport.h ]; then
  pass "#12a: guardian_transport.h present"
else
  fail "#12a: guardian_transport.h missing"
fi

# Verify chrome still runs
out="$(run_chrome --headless --dump-dom https://example.com 2>&1 || true)"
if echo "$out" | grep -q "Example Domain"; then
  pass "#12a: chrome still boots after transport wiring"
else
  fail "#12a: chrome broken after transport wiring"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "[fail] one or more P1-D tests failed"; exit 1
fi
echo "[success] all P1-D tests passed"
```

Run:
```bash
chmod +x /root/byod-browser/build/smoke-test-p1d.sh
```

- [ ] **Step 3: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1d.sh`
Expected: `✗ #12a: guardian_transport.h missing`

- [ ] **Step 4: 创建 chrome/browser/exam/BUILD.gn**

Write `/opt/chromium/src/chrome/browser/exam/BUILD.gn`:

```gn
# BYOD patch #12/#13/#14 (spec §3.1): exam-mode support code.
# All targets here are compile-time linked into chrome_browser; strategy /
# business logic lives in L3 (P5) and L4 (P6), not here.

source_set("exam") {
  sources = [
    "ipc/guardian_transport.cc",
    "ipc/guardian_transport.h",
  ]

  deps = [
    "//base",
  ]

  public_deps = []

  # No visibility restriction: chrome/browser code freely uses these.
  visibility = [
    "//chrome/browser/*",
    "//chrome/browser:*",
  ]
}
```

- [ ] **Step 5: 创建 guardian_transport.h**

Write `/opt/chromium/src/chrome/browser/exam/ipc/guardian_transport.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #12a (spec §3.1, §5.1): abstract transport between the Browser
// process and the native Guardian helper process. Real implementation
// (pipe/socketpair-backed) is provided by P2. This file only defines the
// interface and a Null default so vanilla Chromium behaviour is unchanged.

#ifndef CHROME_BROWSER_EXAM_IPC_GUARDIAN_TRANSPORT_H_
#define CHROME_BROWSER_EXAM_IPC_GUARDIAN_TRANSPORT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/functional/callback.h"

namespace chrome::exam {

enum class MessageType : uint32_t {
  kHandshakeHello = 1,
  kHandshakeAck = 2,
  kHeartbeat = 10,
  kChallenge = 20,
  kChallengeResponse = 21,
  kEvent = 30,          // Guardian → Browser event report.
  kBurnCommand = 40,    // Browser → Guardian: exam is burning.
};

// Wire frame between Browser and Guardian. P1-D uses zero HMAC; P2 replaces
// with real HMAC-SHA256 computed via the session-derived IPC key.
struct GuardianFrame {
  uint64_t seq = 0;
  MessageType type = MessageType::kHeartbeat;
  std::vector<uint8_t> payload;
  std::vector<uint8_t> hmac;  // empty in P1-D; 32 bytes in P2.
};

class GuardianTransport {
 public:
  using OnFrameCallback =
      base::RepeatingCallback<void(const GuardianFrame&)>;

  // Send a frame; non-blocking. Returns false if transport is not connected.
  virtual bool Send(const GuardianFrame& frame) = 0;

  // Register frame receive callback (called on UI thread).
  virtual void SetOnFrameCallback(OnFrameCallback cb) = 0;

  // True if the pipe is up and Guardian is alive.
  virtual bool IsConnected() const = 0;

  virtual ~GuardianTransport() = default;
};

// Install a transport. Default is a NullTransport (no-op).
void SetGuardianTransport(std::unique_ptr<GuardianTransport> transport);
GuardianTransport* GetGuardianTransport();

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_IPC_GUARDIAN_TRANSPORT_H_
```

- [ ] **Step 6: 创建 guardian_transport.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/guardian_transport.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/ipc/guardian_transport.h"

#include "base/no_destructor.h"

namespace chrome::exam {

namespace {

class NullTransport : public GuardianTransport {
 public:
  bool Send(const GuardianFrame&) override { return false; }
  void SetOnFrameCallback(OnFrameCallback) override {}
  bool IsConnected() const override { return false; }
};

std::unique_ptr<GuardianTransport>& MutableTransport() {
  static base::NoDestructor<std::unique_ptr<GuardianTransport>> transport(
      std::make_unique<NullTransport>());
  return *transport;
}

}  // namespace

void SetGuardianTransport(std::unique_ptr<GuardianTransport> transport) {
  if (transport)
    MutableTransport() = std::move(transport);
  else
    MutableTransport() = std::make_unique<NullTransport>();
}

GuardianTransport* GetGuardianTransport() {
  return MutableTransport().get();
}

}  // namespace chrome::exam
```

**注意**：此处 `MutableTransport` **不加锁** —— 因为 spec §5.1.1 规定 IPC 握手在 `PreCreateThreads` 完成，此时只有主线程；`SetGuardianTransport` 只会被 browser main 在启动时调用一次。若 P2 之后有多线程读取，本 sub-plan 冻结的接口允许 P2 在 real transport 内部加锁。

- [ ] **Step 7: 修改 chrome/browser/BUILD.gn 加 dep**

在 `/opt/chromium/src/chrome/browser/BUILD.gn` 里，找到 `static_library("browser")` 或 `component("browser")` target 的 `deps = [ ... ]` 列表，加入：

```gn
    "//chrome/browser/exam",
```

- [ ] **Step 8: 编译**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
```
**若报"link error, exam target not found"**：确认 Step 7 的 dep 加对了 target。**若报"chrome::exam undefined"**：检查 include path 和 namespace 拼写。

- [ ] **Step 9: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
```
Expected: 全通过；`#12a` 显示 `✓ present` 和 `✓ chrome still boots`

- [ ] **Step 10: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/ipc/guardian_transport.h \
        chrome/browser/exam/ipc/guardian_transport.cc \
        chrome/browser/BUILD.gn
git commit -m "byod: add GuardianTransport interface + NullTransport (patch #12a)

Patch #12a (spec §3.1, §5.1). Abstract Browser↔Guardian IPC transport.
P1-D provides NullTransport (Send/IsConnected always false). P2 will
replace with a real pipe/socketpair transport."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0600-add-guardian-transport-interface.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /root/byod-browser
git add patches/0600-add-guardian-transport-interface.patch build/smoke-test-p1d.sh
git commit -m "patch: 0600 GuardianTransport interface (patch #12a)

Est. LOC: ~120 (BUILD.gn + .h + .cc + browser BUILD.gn dep)."
```

---

## Task A2: 补丁 #12b — 心跳状态机

**Files:**
- Create (in Chromium src): `chrome/browser/exam/ipc/heartbeat_state_machine.h`
- Create (in Chromium src): `chrome/browser/exam/ipc/heartbeat_state_machine.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn` — 加新文件到 sources
- Create: `/root/byod-browser/patches/0601-add-heartbeat-state-machine.patch`

**Interfaces:**
- Consumes: Task A1 完成
- **Produces**（P2 与 P5 消费）:
  ```cpp
  namespace chrome::exam {

  // States as per spec §5.1.4.
  enum class HeartbeatState {
    kHealthy,   // Last heartbeat within 5s.
    kDegraded,  // > 8s but < 24s.
    kCritical,  // > 24s but < 48s. UI displays warning.
    kBurn,      // > 48s or wall-clock > 5min. Session must burn.
  };

  class HeartbeatStateMachine {
   public:
    // Beat interval and thresholds match spec §5.1.4.
    static constexpr base::TimeDelta kInterval = base::Seconds(5);
    static constexpr base::TimeDelta kDegradedThreshold = base::Seconds(8);
    static constexpr base::TimeDelta kCriticalThreshold = base::Seconds(24);
    static constexpr base::TimeDelta kBurnThreshold = base::Seconds(48);
    static constexpr base::TimeDelta kWallClockBurnLimit = base::Minutes(5);

    HeartbeatStateMachine();

    // Call from the transport receive callback when a valid heartbeat arrives.
    // Uses monotonic ticks internally, unaffected by system sleep/NTP.
    void RecordHeartbeat();

    // Call from a timer (e.g. every 1s) to update state based on elapsed time.
    // Returns the new state.
    HeartbeatState Tick();

    // Called by main when system sleep/wake events happen — resets the
    // monotonic clock and wall-clock references (per spec §5.1.4).
    void OnSystemResume();
    void OnSystemSuspend();

    HeartbeatState state() const { return state_; }

   private:
    HeartbeatState state_ = HeartbeatState::kHealthy;
    base::TimeTicks last_heartbeat_monotonic_;
    base::Time wall_clock_last_ok_;
    bool suspended_ = false;
  };

  }  // namespace chrome::exam
  ```

- [ ] **Step 1: 追加 smoke test #12b**

Edit `/root/byod-browser/build/smoke-test-p1d.sh`，在 `if [ "$FAILED" ...` 之前追加：

```bash
# ---------- Patch #12b: heartbeat state machine present ----------
echo "[test] patch #12b: HeartbeatStateMachine header"
if [ -f /opt/chromium/src/chrome/browser/exam/ipc/heartbeat_state_machine.h ]; then
  pass "#12b: heartbeat_state_machine.h present"
else
  fail "#12b: heartbeat_state_machine.h missing"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1d.sh`
Expected: `✗ #12b: heartbeat_state_machine.h missing`

- [ ] **Step 3: 创建 heartbeat_state_machine.h**

Write `/opt/chromium/src/chrome/browser/exam/ipc/heartbeat_state_machine.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #12b (spec §5.1.4): 4-state heartbeat FSM (Healthy → Degraded
// → Critical → Burn). Uses base::TimeTicks (monotonic, sleep/NTP-safe).
// State transitions are pure functions of elapsed time; no side effects.

#ifndef CHROME_BROWSER_EXAM_IPC_HEARTBEAT_STATE_MACHINE_H_
#define CHROME_BROWSER_EXAM_IPC_HEARTBEAT_STATE_MACHINE_H_

#include "base/time/time.h"

namespace chrome::exam {

enum class HeartbeatState {
  kHealthy,
  kDegraded,
  kCritical,
  kBurn,
};

class HeartbeatStateMachine {
 public:
  // Constants per spec §5.1.4.
  static constexpr base::TimeDelta kInterval = base::Seconds(5);
  static constexpr base::TimeDelta kDegradedThreshold = base::Seconds(8);
  static constexpr base::TimeDelta kCriticalThreshold = base::Seconds(24);
  static constexpr base::TimeDelta kBurnThreshold = base::Seconds(48);
  static constexpr base::TimeDelta kWallClockBurnLimit = base::Minutes(5);

  HeartbeatStateMachine();

  // Called when a valid heartbeat arrives.
  void RecordHeartbeat();

  // Called periodically (~1 Hz) to recompute state from elapsed time.
  // Returns the new state.
  HeartbeatState Tick();

  // System sleep/wake hooks (per spec §5.1.4): pause degradation on suspend,
  // reset references on resume.
  void OnSystemResume();
  void OnSystemSuspend();

  HeartbeatState state() const { return state_; }

 private:
  HeartbeatState state_;
  base::TimeTicks last_heartbeat_monotonic_;
  base::Time wall_clock_last_ok_;
  bool suspended_;
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_IPC_HEARTBEAT_STATE_MACHINE_H_
```

- [ ] **Step 4: 创建 heartbeat_state_machine.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/heartbeat_state_machine.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/ipc/heartbeat_state_machine.h"

namespace chrome::exam {

HeartbeatStateMachine::HeartbeatStateMachine()
    : state_(HeartbeatState::kHealthy),
      last_heartbeat_monotonic_(base::TimeTicks::Now()),
      wall_clock_last_ok_(base::Time::Now()),
      suspended_(false) {}

void HeartbeatStateMachine::RecordHeartbeat() {
  last_heartbeat_monotonic_ = base::TimeTicks::Now();
  wall_clock_last_ok_ = base::Time::Now();
  if (state_ != HeartbeatState::kBurn) {
    // Burn is terminal per spec §5.1.4.
    state_ = HeartbeatState::kHealthy;
  }
}

HeartbeatState HeartbeatStateMachine::Tick() {
  if (state_ == HeartbeatState::kBurn) return state_;
  if (suspended_) return state_;

  base::TimeDelta since = base::TimeTicks::Now() - last_heartbeat_monotonic_;
  base::TimeDelta wall_since = base::Time::Now() - wall_clock_last_ok_;

  if (wall_since >= kWallClockBurnLimit) {
    state_ = HeartbeatState::kBurn;
  } else if (since >= kBurnThreshold) {
    state_ = HeartbeatState::kBurn;
  } else if (since >= kCriticalThreshold) {
    state_ = HeartbeatState::kCritical;
  } else if (since >= kDegradedThreshold) {
    if (state_ != HeartbeatState::kCritical) {
      state_ = HeartbeatState::kDegraded;
    }
  } else {
    state_ = HeartbeatState::kHealthy;
  }
  return state_;
}

void HeartbeatStateMachine::OnSystemSuspend() {
  suspended_ = true;
}

void HeartbeatStateMachine::OnSystemResume() {
  suspended_ = false;
  // Reset references so a wake doesn't immediately fire Degraded.
  last_heartbeat_monotonic_ = base::TimeTicks::Now();
  wall_clock_last_ok_ = base::Time::Now();
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 修改 BUILD.gn 加入新文件**

在 `/opt/chromium/src/chrome/browser/exam/BUILD.gn` 的 `sources = [` 列表加入：

```gn
    "ipc/heartbeat_state_machine.cc",
    "ipc/heartbeat_state_machine.h",
```

- [ ] **Step 6: 编译**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
```

- [ ] **Step 7: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
```

- [ ] **Step 8: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/ipc/heartbeat_state_machine.h \
        chrome/browser/exam/ipc/heartbeat_state_machine.cc
git commit -m "byod: add 4-state HeartbeatStateMachine (patch #12b)

Patch #12b (spec §5.1.4). Healthy → Degraded (8s) → Critical (24s) →
Burn (48s or wall-clock >5min). Monotonic clock; sleep/wake aware.
Pure state, no side effects; called by transport receive path (P2)
and by session state machine (Task B2)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0601-add-heartbeat-state-machine.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /root/byod-browser
git add patches/0601-add-heartbeat-state-machine.patch build/smoke-test-p1d.sh
git commit -m "patch: 0601 heartbeat state machine (patch #12b)

Est. LOC: ~120."
```

---

## Task A3: 补丁 #12c — Guardian launcher 桩 + browser_main 挂钩

**Files:**
- Create (in Chromium src): `chrome/browser/exam/ipc/guardian_launcher.h`
- Create (in Chromium src): `chrome/browser/exam/ipc/guardian_launcher.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Modify (in Chromium src): `chrome/browser/chrome_browser_main.cc` — 在 `PreCreateThreads` 阶段调用
- Create: `/root/byod-browser/patches/0602-wire-guardian-launch-in-browser-main.patch`

**Interfaces:**
- Consumes: Task A2 完成
- **Produces**（P2 覆写）:
  ```cpp
  namespace chrome::exam {

  // Result of a launch attempt.
  enum class GuardianLaunchStatus {
    kNotAttempted,    // --byod-launch-guardian flag absent; skipped.
    kSuccess,         // Guardian process spawned and pipe open.
    kExecutableMissing,
    kFailedToSpawn,
  };

  // Compute the Guardian executable path — same directory as the current
  // Browser exe, filename "exam_guardian" (with ".exe" on Windows).
  base::FilePath GetGuardianExecutablePath();

  // Attempt to launch Guardian. In P1-D this is a stub that only checks
  // executable presence and returns; P2 replaces with a real spawn +
  // handle/socketpair inheritance + key exchange.
  GuardianLaunchStatus TryLaunchGuardian();

  }  // namespace chrome::exam
  ```

- [ ] **Step 1: 追加 smoke test #12c**

Edit `/root/byod-browser/build/smoke-test-p1d.sh`:

```bash
# ---------- Patch #12c: Guardian launcher stub wired into browser main ----------
echo "[test] patch #12c: launcher stub wired"

if [ -f /opt/chromium/src/chrome/browser/exam/ipc/guardian_launcher.h ]; then
  pass "#12c: guardian_launcher.h present"
else
  fail "#12c: guardian_launcher.h missing"
fi

# Verify chrome still starts even when guardian executable is absent (stub
# should log a message and continue).
out="$(run_chrome --headless --dump-dom https://example.com 2>&1 || true)"
if echo "$out" | grep -q "Example Domain"; then
  pass "#12c: chrome boots even without exam_guardian binary"
else
  fail "#12c: chrome fails to boot without exam_guardian"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1d.sh`

- [ ] **Step 3: 创建 guardian_launcher.h**

Write `/opt/chromium/src/chrome/browser/exam/ipc/guardian_launcher.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #12c (spec §5.1.1): stub Guardian launcher. In P1-D we only
// resolve the expected exam_guardian path and check whether the binary
// exists. Real spawn (with handle inheritance / socketpair, key exchange,
// parent-PID attest) is done by P2.

#ifndef CHROME_BROWSER_EXAM_IPC_GUARDIAN_LAUNCHER_H_
#define CHROME_BROWSER_EXAM_IPC_GUARDIAN_LAUNCHER_H_

#include "base/files/file_path.h"

namespace chrome::exam {

enum class GuardianLaunchStatus {
  kNotAttempted,
  kSuccess,
  kExecutableMissing,
  kFailedToSpawn,
};

// Returns the expected path to exam_guardian, resolved as sibling of the
// current chrome executable. On Windows appends ".exe".
base::FilePath GetGuardianExecutablePath();

// Attempt to launch Guardian. P1-D stub: check presence only. P2 will
// override this behaviour via a compile-time replacement or by installing
// a real transport before this stub runs.
//
// Called from ChromeBrowserMainParts::PreCreateThreads.
GuardianLaunchStatus TryLaunchGuardian();

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_IPC_GUARDIAN_LAUNCHER_H_
```

- [ ] **Step 4: 创建 guardian_launcher.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/guardian_launcher.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/ipc/guardian_launcher.h"

#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/path_service.h"

namespace chrome::exam {

base::FilePath GetGuardianExecutablePath() {
  base::FilePath exe_path;
  if (!base::PathService::Get(base::FILE_EXE, &exe_path)) {
    return base::FilePath();
  }
  base::FilePath dir = exe_path.DirName();
#if BUILDFLAG(IS_WIN)
  return dir.AppendASCII("exam_guardian.exe");
#else
  return dir.AppendASCII("exam_guardian");
#endif
}

GuardianLaunchStatus TryLaunchGuardian() {
  base::FilePath path = GetGuardianExecutablePath();
  if (path.empty()) {
    LOG(WARNING) << "[byod] Guardian path could not be resolved.";
    return GuardianLaunchStatus::kExecutableMissing;
  }
  if (!base::PathExists(path)) {
    // P1-D behaviour: log and continue with NullTransport. Real production
    // build (P10) ships exam_guardian alongside chrome. During P1 development
    // the binary won't exist yet, so we tolerate its absence to keep the dev
    // loop unblocked.
    LOG(WARNING) << "[byod] Guardian binary not found at " << path.value()
                 << " — continuing with NullTransport (P1-D stub).";
    return GuardianLaunchStatus::kExecutableMissing;
  }

  // P1-D stub: DO NOT actually spawn. P2 replaces this function with real
  // launcher (base::LaunchProcess + handle inheritance + key exchange).
  LOG(INFO) << "[byod] Guardian binary found at " << path.value()
            << " — actual spawn deferred to P2.";
  return GuardianLaunchStatus::kNotAttempted;
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 修改 BUILD.gn**

在 `chrome/browser/exam/BUILD.gn` sources 加：

```gn
    "ipc/guardian_launcher.cc",
    "ipc/guardian_launcher.h",
```

deps 加（若原来没 base）：

```gn
    "//base",
```

- [ ] **Step 6: 修改 chrome_browser_main.cc — PreCreateThreads 调用**

在 `/opt/chromium/src/chrome/browser/chrome_browser_main.cc` 顶部 include 区域添加：

```cpp
#include "chrome/browser/exam/ipc/guardian_launcher.h"
```

找到 `ChromeBrowserMainParts::PreCreateThreads()` 或 `PreCreateThreadsImpl` 函数体，在函数末尾（`return content::RESULT_CODE_NORMAL_EXIT;` 之前）添加：

```cpp
  // BYOD patch #12c (spec §5.1.1): attempt to launch Guardian before any
  // renderer / network process is created, so Guardian can attest and monitor
  // subsequent child processes. Failure here is non-fatal in P1-D; P2 will
  // upgrade the status handling.
  chrome::exam::TryLaunchGuardian();
```

- [ ] **Step 7: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**若 `PreCreateThreads` 的函数签名不同**（有的 Chromium 版本叫 `PreCreateThreadsForBrowserProcess` 之类），用 `grep -n "PreCreateThreads" /opt/chromium/src/chrome/browser/chrome_browser_main.cc` 定位实际入口。

- [ ] **Step 8: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
```
Expected: 全通过；chrome 启动时打印 `[byod] Guardian binary not found ... continuing with NullTransport` 到 stderr（因为 exam_guardian 还没构建），但不阻塞启动

- [ ] **Step 9: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/ipc/guardian_launcher.h \
        chrome/browser/exam/ipc/guardian_launcher.cc \
        chrome/browser/chrome_browser_main.cc
git commit -m "byod: stub Guardian launcher in PreCreateThreads (patch #12c)

Patch #12c (spec §5.1.1). Stub only: resolves expected sibling path
(exam_guardian[.exe]) and logs presence. Real spawn/handle-inherit/
key-exchange in P2. Absent binary is tolerated (LOG(WARNING))."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0602-wire-guardian-launch-in-browser-main.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /root/byod-browser
git add patches/0602-wire-guardian-launch-in-browser-main.patch build/smoke-test-p1d.sh
git commit -m "patch: 0602 Guardian launcher stub + browser main hook (patch #12c)

Est. LOC: ~130."
```

---

# §B · Exam Session（补丁 #13）

## Task B1: 补丁 #13a — ExamSession 单例

**Files:**
- Create (in Chromium src): `chrome/browser/exam/session/exam_session.h`
- Create (in Chromium src): `chrome/browser/exam/session/exam_session.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0700-add-exam-session-singleton.patch`

**Interfaces:**
- Consumes: Task A3 完成
- **Produces**（P5 与 P6 消费）:
  ```cpp
  namespace chrome::exam {

  class ExamSession {
   public:
    static ExamSession& Get();  // Global singleton, browser process only.

    // State query.
    SessionState state() const;

    // Getters wired in Task B2 (state machine).

   private:
    ExamSession();
    ~ExamSession() = default;
  };

  }  // namespace chrome::exam
  ```

- [ ] **Step 1: 追加 smoke test #13a**

Edit `/root/byod-browser/build/smoke-test-p1d.sh`:

```bash
# ---------- Patch #13a: ExamSession header present ----------
echo "[test] patch #13a: ExamSession singleton"
if [ -f /opt/chromium/src/chrome/browser/exam/session/exam_session.h ]; then
  pass "#13a: exam_session.h present"
else
  fail "#13a: exam_session.h missing"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1d.sh`

- [ ] **Step 3: 创建 exam_session.h（含前置声明 SessionState，在 Task B2 展开）**

Write `/opt/chromium/src/chrome/browser/exam/session/exam_session.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #13a (spec §3.1, §4.1.2): global ExamSession singleton for the
// Browser process. State machine (Task B2) and burn handling (Task B3)
// extend this file.

#ifndef CHROME_BROWSER_EXAM_SESSION_EXAM_SESSION_H_
#define CHROME_BROWSER_EXAM_SESSION_EXAM_SESSION_H_

namespace chrome::exam {

// Session lifecycle states — expanded in Task B2.
enum class SessionState {
  kIdle,     // Chrome launched, no exam active.
  kActive,   // Exam in progress; Kiosk + monitoring engaged.
  kBurning,  // Burn triggered; UI displays terminal message.
  kEnded,    // Exam ended cleanly (submitted).
};

class ExamSession {
 public:
  // Global singleton accessor. Browser process only. Created lazily on first
  // call.
  static ExamSession& Get();

  SessionState state() const { return state_; }

 private:
  ExamSession();
  ~ExamSession() = default;

  ExamSession(const ExamSession&) = delete;
  ExamSession& operator=(const ExamSession&) = delete;

  SessionState state_ = SessionState::kIdle;

  // Task B2 adds state-machine methods.
  // Task B3 adds burn coordination.
  friend class ExamSessionStateMachineTest;  // for future unit test hooks
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_SESSION_EXAM_SESSION_H_
```

- [ ] **Step 4: 创建 exam_session.cc**

Write `/opt/chromium/src/chrome/browser/exam/session/exam_session.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/session/exam_session.h"

#include "base/no_destructor.h"

namespace chrome::exam {

// static
ExamSession& ExamSession::Get() {
  static base::NoDestructor<ExamSession> instance;
  return *instance;
}

ExamSession::ExamSession() = default;

}  // namespace chrome::exam
```

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "session/exam_session.cc",
    "session/exam_session.h",
```

- [ ] **Step 6: 编译 + smoke tests + 导出 + 验证 + commit**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/session/exam_session.h \
        chrome/browser/exam/session/exam_session.cc
git commit -m "byod: add ExamSession singleton skeleton (patch #13a)

Patch #13a (spec §3.1, §4.1.2). NoDestructor singleton with SessionState
enum. State machine methods added in #13b."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0700-add-exam-session-singleton.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /root/byod-browser
git add patches/0700-add-exam-session-singleton.patch build/smoke-test-p1d.sh
git commit -m "patch: 0700 ExamSession singleton (patch #13a)

Est. LOC: ~50."
```

---

## Task B2: 补丁 #13b — Session 状态机 + 熔断 API

**Files:**
- Modify (in Chromium src): `chrome/browser/exam/session/exam_session.h` — 扩展 API
- Modify (in Chromium src): `chrome/browser/exam/session/exam_session.cc` — 实现方法
- Create: `/root/byod-browser/patches/0701-add-exam-session-state-machine.patch`

**Interfaces:**
- Consumes: Task B1 完成
- **Produces**（P5 消费 `TriggerBurn` / `Start`；P6 消费所有 API）:
  ```cpp
  // Additions to ExamSession:
  void StartExam(std::string_view session_id);  // Idle → Active
  void EndExam();                                // Active → Ended
  void TriggerBurn(std::string_view reason);     // Active/Critical → Burning

  const std::string& session_id() const;
  const std::string& burn_reason() const;

  // Observer for state changes (used by BrowserView to display UI, by P8 to
  // emit event, etc.)
  using StateChangeObserver =
      base::RepeatingCallback<void(SessionState old_state,
                                   SessionState new_state)>;
  void AddObserver(StateChangeObserver cb);
  ```

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1d.sh`:

```bash
# ---------- Patch #13b: session state machine ----------
echo "[test] patch #13b: ExamSession state transitions"
# Compile-time only: the extended header must include TriggerBurn declaration.
if grep -q "TriggerBurn" /opt/chromium/src/chrome/browser/exam/session/exam_session.h; then
  pass "#13b: TriggerBurn declared"
else
  fail "#13b: TriggerBurn missing from header"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1d.sh`

- [ ] **Step 3: 扩展 exam_session.h**

Edit `/opt/chromium/src/chrome/browser/exam/session/exam_session.h`：

在 `public:` 块内、`state()` getter 之后添加：

```cpp
  // State transitions.
  void StartExam(std::string_view session_id);  // kIdle -> kActive
  void EndExam();                               // kActive -> kEnded
  void TriggerBurn(std::string_view reason);    // any non-terminal -> kBurning

  const std::string& session_id() const { return session_id_; }
  const std::string& burn_reason() const { return burn_reason_; }

  // Observer for state changes. Called synchronously on the UI thread after
  // each transition.
  using StateChangeObserver =
      base::RepeatingCallback<void(SessionState old_state,
                                   SessionState new_state)>;
  void AddObserver(StateChangeObserver cb);
```

在 `private:` 块 `state_` 之后添加：

```cpp
  std::string session_id_;
  std::string burn_reason_;
  std::vector<StateChangeObserver> observers_;

  void SetState(SessionState new_state);
```

**顶部 include 需要**：

```cpp
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
```

- [ ] **Step 4: 实现 exam_session.cc**

Edit `/opt/chromium/src/chrome/browser/exam/session/exam_session.cc`，添加实现：

```cpp
void ExamSession::StartExam(std::string_view session_id) {
  if (state_ != SessionState::kIdle) return;  // Idempotent guard.
  session_id_ = std::string(session_id);
  SetState(SessionState::kActive);
}

void ExamSession::EndExam() {
  if (state_ != SessionState::kActive) return;  // Only from Active.
  SetState(SessionState::kEnded);
}

void ExamSession::TriggerBurn(std::string_view reason) {
  // Burning is terminal — once set, cannot leave.
  if (state_ == SessionState::kBurning || state_ == SessionState::kEnded)
    return;
  burn_reason_ = std::string(reason);
  SetState(SessionState::kBurning);
}

void ExamSession::AddObserver(StateChangeObserver cb) {
  observers_.push_back(std::move(cb));
}

void ExamSession::SetState(SessionState new_state) {
  if (new_state == state_) return;
  SessionState old = state_;
  state_ = new_state;
  for (const auto& obs : observers_) {
    obs.Run(old, new_state);
  }
}
```

- [ ] **Step 5: 编译 + smoke + 导出 + 验证 + commit**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /opt/chromium/src
git add chrome/browser/exam/session/exam_session.h \
        chrome/browser/exam/session/exam_session.cc
git commit -m "byod: add state machine + observer to ExamSession (patch #13b)

Patch #13b (spec §3.1, §4.1.2). Transitions: kIdle → kActive (StartExam),
kActive → kEnded (EndExam), any non-terminal → kBurning (TriggerBurn).
Burning is terminal per spec §4.1.2. Observers notified synchronously
on UI thread."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0701-add-exam-session-state-machine.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /root/byod-browser
git add patches/0701-add-exam-session-state-machine.patch build/smoke-test-p1d.sh
git commit -m "patch: 0701 exam session state machine (patch #13b)

Est. LOC: ~80."
```

---

## Task B3: 补丁 #13c — 熔断信号→ Browser 允许关闭

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/browser.cc` — 观察 ExamSession 状态
- Modify (in Chromium src): `chrome/browser/ui/browser.h` — 加成员
- Create: `/root/byod-browser/patches/0702-wire-burn-signal-to-browser-close.patch`

**Interfaces:**
- Consumes: Task B2 完成 + P1-B Task 9 的 `SetByodAllowClose` API
- Produces: 当 `ExamSession::TriggerBurn` 被调用（state → kBurning）时，Browser 自动设置 `byod_allow_close_ = true`；用户接下来可以正常关闭窗口（P6 会渲染熔断 UI 并引导用户关闭）；正常 `EndExam` 也允许关闭

**说明**：本 patch 打通"熔断状态 → Browser 可关闭"的路径。真正的熔断 UI（占满全屏的"请联系考试老师"页面）由 P6 交付；本 patch 只保证熔断时窗口可关，避免学生被卡住。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1d.sh`:

```bash
# ---------- Patch #13c: burn signal enables close ----------
echo "[test] patch #13c: burn wires to allow-close"
# Compile-time only: browser.cc includes exam_session.h
if grep -q "exam_session.h" /opt/chromium/src/chrome/browser/ui/browser.cc; then
  pass "#13c: browser.cc references exam_session"
else
  fail "#13c: browser.cc missing exam_session include"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1d.sh`

- [ ] **Step 3: 修改 browser.cc — 构造时注册 observer**

在 `/opt/chromium/src/chrome/browser/ui/browser.cc` 顶部 include 区域添加：

```cpp
#include "chrome/browser/exam/session/exam_session.h"
```

在 `Browser::Browser(...)` 构造函数体末尾添加：

```cpp
  // BYOD patch #13c (spec §4.1.2): observe exam session state so that a
  // burn or clean-end automatically flips byod_allow_close_ to true, letting
  // the user close the (otherwise Kiosk-locked) window. UI for the burn
  // screen is drawn by exam shell (P6).
  chrome::exam::ExamSession::Get().AddObserver(
      base::BindRepeating(
          [](base::WeakPtr<Browser> browser,
             chrome::exam::SessionState /*old_state*/,
             chrome::exam::SessionState new_state) {
            if (!browser) return;
            using chrome::exam::SessionState;
            if (new_state == SessionState::kBurning ||
                new_state == SessionState::kEnded) {
              browser->SetByodAllowClose(true);
            }
          },
          weak_factory_.GetWeakPtr()));
```

**依赖**：`weak_factory_` 是 `Browser` 类的既有成员（Chromium 大多数长期存活对象都有 WeakPtrFactory）。若名称不同，用 `base::Unretained(this)` 替代 —— 但那要求 Browser 一定活得比 observer 长，观察者本身可能持有到 ExamSession 单例（永生），所以更好的做法是 WeakPtr。

**若 Browser 没有 WeakPtrFactory**：在 `browser.h` 里加：

```cpp
 private:
  // BYOD patch #13c: for safe callback binding into ExamSession observer.
  base::WeakPtrFactory<Browser> weak_factory_{this};
```

（放在类末尾，`weak_factory_{this}` 需要 `this` 已完整构造，通常放最后）

- [ ] **Step 4: 编译 + smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
```

- [ ] **Step 5: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/browser.cc chrome/browser/ui/browser.h
git commit -m "byod: wire ExamSession burn signal to allow-close (patch #13c)

Patch #13c (spec §4.1.2). Browser observes ExamSession state transitions;
kBurning or kEnded flips byod_allow_close_ to true, releasing the
Kiosk-close lock so the user can exit."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0702-wire-burn-signal-to-browser-close.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /root/byod-browser
git add patches/0702-wire-burn-signal-to-browser-close.patch build/smoke-test-p1d.sh
git commit -m "patch: 0702 burn signal to allow-close (patch #13c)

Est. LOC: ~40."
```

---

# §C · 事件采集（补丁 #14）

## Task C1: 补丁 #14a — ExamEventSink 接口

**Files:**
- Create (in Chromium src): `chrome/browser/exam/events/exam_event.h`
- Create (in Chromium src): `chrome/browser/exam/events/exam_event_sink.h`
- Create (in Chromium src): `chrome/browser/exam/events/exam_event_sink.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0800-add-exam-event-sink-interface.patch`

**Interfaces:**
- Consumes: Task B3 完成
- **Produces**（P5 消费 sink 实现；P8 消费 event schema）:
  ```cpp
  namespace chrome::exam {

  enum class ExamEventType : uint32_t {
    kWindowBlur = 1,
    kWindowFocus = 2,
    kWindowResize = 3,
    kNavigation = 10,
    kNavigationBlocked = 11,
    // more added by later phases.
  };

  struct ExamEvent {
    ExamEventType type;
    base::TimeTicks monotonic_ts;   // For ordering.
    base::Time wall_ts;             // For human-readable log.
    std::string payload_json;       // Type-specific JSON blob.
  };

  class ExamEventSink {
   public:
    // Called on the UI thread when a new event is emitted.
    virtual void OnEvent(const ExamEvent& event) = 0;
    virtual ~ExamEventSink() = default;
  };

  void SetExamEventSink(std::unique_ptr<ExamEventSink> sink);
  ExamEventSink* GetExamEventSink();

  // Convenience: emit an event via the installed sink. No-op if none.
  void EmitEvent(ExamEventType type, std::string payload_json);

  }  // namespace chrome::exam
  ```

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1d.sh`:

```bash
# ---------- Patch #14a: event sink interface ----------
echo "[test] patch #14a: ExamEventSink header"
if [ -f /opt/chromium/src/chrome/browser/exam/events/exam_event_sink.h ]; then
  pass "#14a: exam_event_sink.h present"
else
  fail "#14a: exam_event_sink.h missing"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1d.sh`

- [ ] **Step 3: 创建 exam_event.h**

Write `/opt/chromium/src/chrome/browser/exam/events/exam_event.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #14a (spec §3.1, §5.3): event schema shared between the
// Chromium-side event forwarders (#14b/#14c) and the L3 event sink (P5).

#ifndef CHROME_BROWSER_EXAM_EVENTS_EXAM_EVENT_H_
#define CHROME_BROWSER_EXAM_EVENTS_EXAM_EVENT_H_

#include <cstdint>
#include <string>

#include "base/time/time.h"

namespace chrome::exam {

enum class ExamEventType : uint32_t {
  kWindowBlur = 1,
  kWindowFocus = 2,
  kWindowResize = 3,
  kNavigation = 10,
  kNavigationBlocked = 11,
  // Additional types added in P3/P4/P5.
};

struct ExamEvent {
  ExamEventType type = ExamEventType::kWindowBlur;
  base::TimeTicks monotonic_ts;
  base::Time wall_ts;
  std::string payload_json;  // Free-form JSON; schema-per-type doc in P8.
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_EVENTS_EXAM_EVENT_H_
```

- [ ] **Step 4: 创建 exam_event_sink.h**

Write `/opt/chromium/src/chrome/browser/exam/events/exam_event_sink.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #14a (spec §3.1, §5.3): abstract sink for exam events. Default
// is null (events dropped). L3 (P5) installs a real sink that batches to
// Session Report (P8).

#ifndef CHROME_BROWSER_EXAM_EVENTS_EXAM_EVENT_SINK_H_
#define CHROME_BROWSER_EXAM_EVENTS_EXAM_EVENT_SINK_H_

#include <memory>
#include <string>

#include "chrome/browser/exam/events/exam_event.h"

namespace chrome::exam {

class ExamEventSink {
 public:
  // Called on the UI thread. Sinks should not block; heavy work goes to a
  // background sequence.
  virtual void OnEvent(const ExamEvent& event) = 0;
  virtual ~ExamEventSink() = default;
};

void SetExamEventSink(std::unique_ptr<ExamEventSink> sink);
ExamEventSink* GetExamEventSink();

// Convenience emitter: builds ExamEvent with fresh timestamps and dispatches
// to the installed sink. No-op if none installed.
void EmitEvent(ExamEventType type, std::string payload_json);

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_EVENTS_EXAM_EVENT_SINK_H_
```

- [ ] **Step 5: 创建 exam_event_sink.cc**

Write `/opt/chromium/src/chrome/browser/exam/events/exam_event_sink.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/events/exam_event_sink.h"

#include "base/no_destructor.h"

namespace chrome::exam {

namespace {

std::unique_ptr<ExamEventSink>& MutableSink() {
  static base::NoDestructor<std::unique_ptr<ExamEventSink>> sink;
  return *sink;
}

}  // namespace

void SetExamEventSink(std::unique_ptr<ExamEventSink> sink) {
  MutableSink() = std::move(sink);
}

ExamEventSink* GetExamEventSink() {
  return MutableSink().get();
}

void EmitEvent(ExamEventType type, std::string payload_json) {
  ExamEventSink* sink = GetExamEventSink();
  if (!sink) return;  // Silent drop.
  ExamEvent event;
  event.type = type;
  event.monotonic_ts = base::TimeTicks::Now();
  event.wall_ts = base::Time::Now();
  event.payload_json = std::move(payload_json);
  sink->OnEvent(event);
}

}  // namespace chrome::exam
```

- [ ] **Step 6: 修改 BUILD.gn**

Sources 加：

```gn
    "events/exam_event.h",
    "events/exam_event_sink.cc",
    "events/exam_event_sink.h",
```

- [ ] **Step 7: 编译 + smoke + 导出 + 验证 + commit**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/events/exam_event.h \
        chrome/browser/exam/events/exam_event_sink.h \
        chrome/browser/exam/events/exam_event_sink.cc
git commit -m "byod: add ExamEventSink interface + EmitEvent helper (patch #14a)

Patch #14a (spec §3.1, §5.3). Abstract sink; default null (drop).
L3 (P5) installs a real sink that batches to Session Report (P8)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0800-add-exam-event-sink-interface.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /root/byod-browser
git add patches/0800-add-exam-event-sink-interface.patch build/smoke-test-p1d.sh
git commit -m "patch: 0800 ExamEventSink interface + EmitEvent (patch #14a)

Est. LOC: ~120."
```

---

## Task C2: 补丁 #14b — 窗口 blur/focus/resize 事件 forwarder

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/views/frame/browser_view.cc` — 挂 delegate 方法
- Create: `/root/byod-browser/patches/0801-forward-window-blur-focus-resize.patch`

**Interfaces:**
- Consumes: Task C1 完成
- Produces: 每次 BrowserView 收到 blur/focus/resize → 调 `EmitEvent(...)`；payload JSON 包含时间戳和 window bounds

- [ ] **Step 1: 定位 BrowserView 的 OnWidgetActivationChanged / OnWidgetBoundsChanged**

Run:
```bash
grep -n "OnWidgetActivationChanged\|OnWidgetBoundsChanged\|OnWidgetVisibilityChanged" /opt/chromium/src/chrome/browser/ui/views/frame/browser_view.cc | head -10
```

- [ ] **Step 2: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1d.sh`:

```bash
# ---------- Patch #14b: window event forwarders wired ----------
echo "[test] patch #14b: BrowserView emits blur/focus/resize"
if grep -q "EmitEvent.*kWindowBlur\|kWindowFocus\|kWindowResize" /opt/chromium/src/chrome/browser/ui/views/frame/browser_view.cc; then
  pass "#14b: emitters present in browser_view.cc"
else
  fail "#14b: browser_view.cc missing event emitters"
fi
```

- [ ] **Step 3: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1d.sh`

- [ ] **Step 4: 修改 browser_view.cc**

在 `/opt/chromium/src/chrome/browser/ui/views/frame/browser_view.cc` 顶部 include 添加：

```cpp
#include "chrome/browser/exam/events/exam_event_sink.h"
```

在 `BrowserView::OnWidgetActivationChanged` 函数体末尾添加：

```cpp
  // BYOD patch #14b (spec §4.1.5): forward blur/focus events to L3 event sink.
  chrome::exam::EmitEvent(
      active ? chrome::exam::ExamEventType::kWindowFocus
             : chrome::exam::ExamEventType::kWindowBlur,
      /*payload_json=*/"{}");
```

（`OnWidgetActivationChanged(views::Widget* widget, bool active)` 的参数名可能不同 —— 用实际的）

在 `BrowserView::OnWidgetBoundsChanged` 函数体末尾添加：

```cpp
  // BYOD patch #14b (spec §4.1.5): forward resize event.
  gfx::Rect bounds = widget->GetWindowBoundsInScreen();
  std::string payload = base::StringPrintf(
      R"({"width":%d,"height":%d,"x":%d,"y":%d})",
      bounds.width(), bounds.height(), bounds.x(), bounds.y());
  chrome::exam::EmitEvent(chrome::exam::ExamEventType::kWindowResize,
                          std::move(payload));
```

**若函数不存在**：需要 override `views::WidgetObserver` 或类似接口；添加 override 声明到 header：

```cpp
  // BYOD patch #14b: exam event forwarding.
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override;
  void OnWidgetBoundsChanged(views::Widget* widget,
                             const gfx::Rect& new_bounds) override;
```

（若已 override 就直接扩展现有实现；grep 结果告诉你实情）

**顶部还要 include**（若尚未 include）：

```cpp
#include "base/strings/stringprintf.h"
```

- [ ] **Step 5: 编译 + smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
```

- [ ] **Step 6: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/views/frame/browser_view.cc \
        chrome/browser/ui/views/frame/browser_view.h
git commit -m "byod: forward window blur/focus/resize to ExamEventSink (patch #14b)

Patch #14b (spec §4.1.5). Every BrowserView activation change and size
change emits a corresponding ExamEvent. Payload JSON schema documented
in P8."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0801-forward-window-blur-focus-resize.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /root/byod-browser
git add patches/0801-forward-window-blur-focus-resize.patch build/smoke-test-p1d.sh
git commit -m "patch: 0801 forward window events (patch #14b)

Est. LOC: ~40."
```

---

## Task C3: 补丁 #14c — Navigation 事件 forwarder

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/browser.cc` — 使用 `WebContentsObserver` 观察 nav
- Create (in Chromium src): `chrome/browser/exam/events/exam_navigation_observer.h`
- Create (in Chromium src): `chrome/browser/exam/events/exam_navigation_observer.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0802-forward-navigation-events.patch`

**Interfaces:**
- Consumes: Task C2 完成
- Produces: 每次 tab 内 navigation 完成或被拦截 → emit `kNavigation` / `kNavigationBlocked` event；payload JSON 含 URL、method、是否 committed

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1d.sh`:

```bash
# ---------- Patch #14c: navigation event forwarder ----------
echo "[test] patch #14c: navigation observer wired"
if [ -f /opt/chromium/src/chrome/browser/exam/events/exam_navigation_observer.h ]; then
  pass "#14c: exam_navigation_observer.h present"
else
  fail "#14c: exam_navigation_observer.h missing"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1d.sh`

- [ ] **Step 3: 创建 exam_navigation_observer.h**

Write `/opt/chromium/src/chrome/browser/exam/events/exam_navigation_observer.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #14c (spec §4.1.5, §5.3): WebContentsObserver that emits
// navigation events to the exam event sink.

#ifndef CHROME_BROWSER_EXAM_EVENTS_EXAM_NAVIGATION_OBSERVER_H_
#define CHROME_BROWSER_EXAM_EVENTS_EXAM_NAVIGATION_OBSERVER_H_

#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace content {
class WebContents;
class NavigationHandle;
}  // namespace content

namespace chrome::exam {

class ExamNavigationObserver
    : public content::WebContentsObserver,
      public content::WebContentsUserData<ExamNavigationObserver> {
 public:
  explicit ExamNavigationObserver(content::WebContents* web_contents);
  ~ExamNavigationObserver() override;

  // WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override;

 private:
  friend class content::WebContentsUserData<ExamNavigationObserver>;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_EVENTS_EXAM_NAVIGATION_OBSERVER_H_
```

- [ ] **Step 4: 创建 exam_navigation_observer.cc**

Write `/opt/chromium/src/chrome/browser/exam/events/exam_navigation_observer.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/events/exam_navigation_observer.h"

#include "base/strings/stringprintf.h"
#include "chrome/browser/exam/events/exam_event_sink.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"

namespace chrome::exam {

ExamNavigationObserver::ExamNavigationObserver(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<ExamNavigationObserver>(*web_contents) {}

ExamNavigationObserver::~ExamNavigationObserver() = default;

void ExamNavigationObserver::DidFinishNavigation(
    content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame()) return;  // Skip subframes for now.

  const bool committed = handle->HasCommitted();
  const std::string url = handle->GetURL().possibly_invalid_spec();
  const int net_error = static_cast<int>(handle->GetNetErrorCode());

  std::string payload = base::StringPrintf(
      R"({"url":%s,"committed":%s,"net_error":%d})",
      base::GetQuotedJSONString(url).c_str(),
      committed ? "true" : "false",
      net_error);

  ExamEventType type =
      committed ? ExamEventType::kNavigation : ExamEventType::kNavigationBlocked;
  EmitEvent(type, std::move(payload));
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(ExamNavigationObserver);

}  // namespace chrome::exam
```

**注意** `base::GetQuotedJSONString` 位于 `base/json/string_escape.h`。若编译报"undefined"，add include：

```cpp
#include "base/json/string_escape.h"
```

- [ ] **Step 5: 修改 browser.cc — 在 tab attach 时装 observer**

在 `/opt/chromium/src/chrome/browser/ui/browser.cc` 顶部 include 添加：

```cpp
#include "chrome/browser/exam/events/exam_navigation_observer.h"
```

找 `Browser::OnTabInsertedAt` 或 `Browser::TabInsertedAt` （grep 定位实际方法名），在函数体末尾添加：

```cpp
  // BYOD patch #14c (spec §4.1.5, §5.3): attach navigation observer to
  // every new tab so all main-frame navigations are recorded.
  chrome::exam::ExamNavigationObserver::CreateForWebContents(contents);
```

（`contents` 是该函数的参数名 —— 用实际的）

- [ ] **Step 6: 修改 BUILD.gn**

Sources 加：

```gn
    "events/exam_navigation_observer.cc",
    "events/exam_navigation_observer.h",
```

deps 加（若尚未有）：

```gn
    "//content/public/browser",
```

- [ ] **Step 7: 编译 + smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
```

**若 `WEB_CONTENTS_USER_DATA_KEY_IMPL` 报错**：这是 Chromium 常见宏，位于 `content/public/browser/web_contents_user_data.h`；某些新版本改成不需要单独 IMPL 宏（直接放在 header 里）—— 按编译提示调整。

- [ ] **Step 8: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/events/exam_navigation_observer.h \
        chrome/browser/exam/events/exam_navigation_observer.cc \
        chrome/browser/ui/browser.cc
git commit -m "byod: attach ExamNavigationObserver to every WebContents (patch #14c)

Patch #14c (spec §4.1.5, §5.3). Each committed or blocked main-frame
navigation emits kNavigation / kNavigationBlocked event with URL,
committed flag, and net error code."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0802-forward-navigation-events.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1d.sh

cd /root/byod-browser
git add patches/0802-forward-navigation-events.patch build/smoke-test-p1d.sh
git commit -m "patch: 0802 navigation event forwarder (patch #14c)

Est. LOC: ~100."
```

---

## Task D1: 端到端集成测试 — 用测试 sink 验证事件流转 + Session 熔断路径

**Files:**
- Create (in Chromium src, temporary): `chrome/browser/exam/events/test_event_sink.cc`
- Modify (in Chromium src, temporary): `chrome/browser/chrome_browser_main.cc` — 装测试 sink
- **不导出 patch** — 纯本地行为验证

**Interfaces:**
- Consumes: Task C3 完成
- Produces: 一次性行为验证 —— 证明 EmitEvent 能到达 sink、TriggerBurn 能让 Browser 变为 AllowClose 状态

- [ ] **Step 1: 写测试 sink，把事件写到 stderr**

Write `/opt/chromium/src/chrome/browser/exam/events/test_event_sink.cc`（临时）:

```cpp
// TEMPORARY: BYOD P1-D Task D1 verification. Delete before commit.
#include "chrome/browser/exam/events/exam_event_sink.h"

#include <iostream>

#include "base/logging.h"

namespace chrome::exam {
namespace {

class LoggingSink : public ExamEventSink {
 public:
  void OnEvent(const ExamEvent& event) override {
    LOG(INFO) << "[byod-test] event type=" << static_cast<uint32_t>(event.type)
              << " payload=" << event.payload_json;
  }
};

}  // namespace

void InstallTestSink() {
  SetExamEventSink(std::make_unique<LoggingSink>());
}

}  // namespace chrome::exam
```

- [ ] **Step 2: 临时挂到 browser main**

在 `chrome_browser_main.cc` 的 `PreCreateThreads` 尾部（Guardian launch 之后）添加：

```cpp
  // TEMP P1-D Task D1 — DELETE after test.
  extern void InstallTestSink();
  chrome::exam::InstallTestSink();
```

BUILD.gn 临时加 `"events/test_event_sink.cc"`。

- [ ] **Step 3: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

- [ ] **Step 4: 启动 chrome，检查 stderr 里有 event 输出**

Run:
```bash
/opt/chromium/src/out/linux/chrome \
  --user-data-dir=/tmp/byod-p1d-t1 \
  --no-first-run --no-sandbox --disable-gpu \
  --enable-logging=stderr --v=0 \
  --headless --dump-dom https://example.com 2>&1 | grep -E "\[byod-test\]"
```
Expected: 至少一条 `event type=10 payload=...` (kNavigation)

**若无输出**：headless 模式下可能 BrowserView 事件没触发（因无窗口）；至少能看到 navigation 事件（因为 WebContents 有 nav）。

- [ ] **Step 5: 移除测试代码**

Run:
```bash
cd /opt/chromium/src
rm chrome/browser/exam/events/test_event_sink.cc
git checkout chrome/browser/chrome_browser_main.cc
git checkout chrome/browser/exam/BUILD.gn
git status
```
Expected: 干净。

- [ ] **Step 6: 再编 + 全量 smoke 验证残留清除**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
```

本 task 不 commit（无产出）。

---

## Task D2: 补丁总量预算校验 + 交付确认

**Files:**
- Create: `/root/byod-browser/docs/development/p1-d-acceptance.md`

**Interfaces:**
- Consumes: Task A1..A3 + B1..B3 + C1..C3 + D1 全部完成
- Produces: P1-D 交付验收 checklist

- [ ] **Step 1: 统计当前 patch 总行数**

Run:
```bash
source /root/byod-browser/build/chromium-version.env
cd /opt/chromium/src
git diff --shortstat "$CHROMIUM_TAG"..HEAD
```
Expected: P1-A + P1-B + P1-C + P1-D 累计 insertions ≤ 2530

- [ ] **Step 2: 数累计补丁文件数**

Run:
```bash
ls /root/byod-browser/patches/*.patch | wc -l
```
Expected: 31（22 + 9）

- [ ] **Step 3: 全量 revert + apply + 编译 + smoke**

Run:
```bash
echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
```
Expected: 全部成功

- [ ] **Step 4: 写 acceptance checklist**

Write `/root/byod-browser/docs/development/p1-d-acceptance.md`:

```markdown
# P1-D 交付验收 Checklist

## 补丁类

- [ ] `patches/0600-add-guardian-transport-interface.patch` 存在
- [ ] `patches/0601-add-heartbeat-state-machine.patch` 存在
- [ ] `patches/0602-wire-guardian-launch-in-browser-main.patch` 存在
- [ ] `patches/0700-add-exam-session-singleton.patch` 存在
- [ ] `patches/0701-add-exam-session-state-machine.patch` 存在
- [ ] `patches/0702-wire-burn-signal-to-browser-close.patch` 存在
- [ ] `patches/0800-add-exam-event-sink-interface.patch` 存在
- [ ] `patches/0801-forward-window-blur-focus-resize.patch` 存在
- [ ] `patches/0802-forward-navigation-events.patch` 存在

## 编译类

- [ ] `bash build/revert-patches.sh && bash build/apply-patches.sh` 无冲突
- [ ] `bash build/build-linux.sh chrome` 成功
- [ ] `git diff --shortstat "$CHROMIUM_TAG"..HEAD` insertions ≤ 2530（P1-A+B+C+D 累计）

## 行为类（Linux headless smoke tests）

- [ ] `bash build/smoke-test.sh` 全部 pass
- [ ] `bash build/smoke-test-p1a.sh` 全部 pass
- [ ] `bash build/smoke-test-p1b.sh` 全部 pass
- [ ] `bash build/smoke-test-p1c.sh` 全部 pass
- [ ] `bash build/smoke-test-p1d.sh` 全部 pass

## 启动日志类

- [ ] Chromium 启动时 stderr 包含 `[byod] Guardian binary not found ... continuing with NullTransport`（因 exam_guardian 未构建，属预期）
- [ ] 启动不因 Guardian 缺失而崩溃

## 端到端集成类（Task D1）

- [ ] 装上 LoggingSink 后，headless 访问 example.com 至少看到一条 `[byod-test] event type=10`（kNavigation）
- [ ] Task D1 结束后测试代码全部 revert，`git status` 干净

## 接口冻结类（供 P2/P5/P6/P8 消费）

- [ ] `chrome::exam::GuardianTransport` / `MessageType` / `GuardianFrame` 已 export
- [ ] `chrome::exam::HeartbeatStateMachine` + 常量 (kInterval=5s, kDegraded=8s, kCritical=24s, kBurn=48s, kWallClockLimit=5min) 已 export
- [ ] `chrome::exam::TryLaunchGuardian` + `GetGuardianExecutablePath` 已 export
- [ ] `chrome::exam::ExamSession::Get()` + `StartExam` / `EndExam` / `TriggerBurn` / `AddObserver` 已 export
- [ ] `chrome::exam::ExamEventSink` / `ExamEvent` / `ExamEventType` / `EmitEvent` 已 export

## 文档类

- [ ] 本文件（p1-d-acceptance.md）存在
- [ ] roadmap 里 P1 状态更新为 `⏳ 进行中 (P1-A + P1-B + P1-C + P1-D 完成)`

## 交付判定

以上全部打勾 → P1-D 完成，可进入 P1-E（尚未编写）。P1-E 后 P1 完结，可进入 P2..P11。
```

- [ ] **Step 5: Commit acceptance + 更新 roadmap**

Run:
```bash
cd /root/byod-browser
git add docs/development/p1-d-acceptance.md
git commit -m "docs: add P1-D acceptance checklist"
```

编辑 `docs/superpowers/plans/2026-07-11-byod-roadmap.md` 把 P1 状态改成 `⏳ 进行中 (P1-A + P1-B + P1-C + P1-D 完成)`，然后：

```bash
cd /root/byod-browser
git add docs/superpowers/plans/2026-07-11-byod-roadmap.md
git commit -m "docs: mark P1-D complete in roadmap"
```

---

## P1-D 完成后

- 已交付 9 个补丁文件（对应 spec §3.1 补丁 #12、#13、#14 各拆细为 3 项）
- 累计 patch 行数应 ≤ 2530（P1-A 600 + P1-B 580 + P1-C 450 + P1-D 900 预算）
- **P1 累计接口冻结**：14 个类/结构体/API 已 export，P2/P3/P4/P5/P6/P7/P8 全部有可对接的钩子
- 下一批：**P1-E**（补丁 #15 硬件指纹接口），预算 ~250 行

**预计实际耗时**（含每次增量编译）：
- 熟练 Chromium 开发者：5-7 天
- 首次改 Chromium 的开发者：2-3 周

**风险与常见坑：**

- **base::NoDestructor 顺序问题**：多个 NoDestructor 单例互相引用时初始化顺序未定义。本 sub-plan 中 `MutableTransport`、`MutableSink`、`ExamSession::Get()` 都是独立 NoDestructor，互不引用（EmitEvent 通过 GetExamEventSink 调用，惰性），OK
- **PreCreateThreads 里做太多事**：Guardian launch 是本 sub-plan 唯一新增在此阶段的动作，且 P1-D 只做 log；真正的 spawn 在 P2 里可能更耗时，届时要评估
- **WebContentsUserData 宏在新版 Chromium 里的差异**：`WEB_CONTENTS_USER_DATA_KEY_IMPL(Class)` 在 Chromium 100+ 有些版本不再需要单独的 IMPL 宏；若编译报错，按提示改成放在类 body 内
- **Observer 生命周期**：ExamSession 是 NoDestructor 单例（永生），Browser 是短寿；本 sub-plan 用 WeakPtr 绑定安全。若某个 sub-plan 忘了 WeakPtr，会 UAF —— code review 重点
- **BUILD.gn deps 循环**：`chrome/browser` deps 加了 `chrome/browser/exam`；`chrome/browser/exam` 若反过来 dep `chrome/browser` 会成环。本 sub-plan 里 `chrome/browser/exam` 只 dep `//base` + `//content/public/browser`，安全
- **Session 熔断后 Kiosk 是否真的能关**：本 sub-plan 只打通"kBurning → SetByodAllowClose(true)"，P1-B 的 `Browser::ShouldCloseWindow` 检查 `byod_allow_close_`；假设 P6 交付前手动测试 —— P1-D 里 headless smoke 覆盖不到 UI 关闭，只能靠代码 review 保证 wiring 正确
