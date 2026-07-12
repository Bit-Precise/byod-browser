# P2-D · Challenge-Response 反 mock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 spec §5.1.5 定义的**周期 challenge-response 反 mock** —— Browser 每 30s 随机挑一个 challenge type 发给 Guardian，Guardian 用**真实系统 API**采集所需信息（活跃 IME 名称、近 30s 新起进程列表、TPM/SE quote、显示器数）后 HMAC 返回；Browser 校验响应，超时或格式错误立即熔断。fake Guardian 因无法采集真实系统数据而被识破。

**Architecture:** 两个新组件在 Browser 侧：
- **`ChallengeIssuer`**：随机选择 challenge type + payload (nonce)、通过 transport 发送、启动 3s 超时定时器、匹配返回的 response frame（按 seq）
- **`ChallengeVerifier`**（**接口 + null 实现**）：给出一个 challenge 与响应 payload，验证响应"看起来合理"（如格式对 + timestamp 合理 + 硬件签名可选校验）；真实的**跨机对账**（比如 Browser 侧独立采集显示器数与 Guardian 报告的对比）留给 P2-E 集成阶段做

Guardian 侧的 challenge responder 在 **P3**（Rust Windows）和 **P4**（Swift macOS）里实现 —— 本 sub-plan 只冻结 challenge payload schema，Guardian 端按 schema 实现即可。

**本 sub-plan 不做**（P2-E 交付）：真实的 challenge-response 端到端联调、challenge 结果与 IME/进程/显示器策略引擎（P5）的对账。

**Tech Stack:** Chromium C++、`base::RepeatingTimer`、`base::RandInt`、`base::Value` (JSON 构造)、gtest with MOCK_TIME。

## Global Constraints

- Chromium 补丁总量 ≤ 3000 行硬预算；累计 ~2820（P1 + P2-A/B/C）
- Chromium 补丁只做"删/禁"和"钩子暴露"；P2-D 是**实现层**，不新增 Chromium 补丁
- 补丁文件命名遵循 P0 patch-workflow.md 序号分段：`0600-0699` IPC；P2-A/B/C 已用 0610-0616、0620-0623、0630-0633；P2-D 用 **0640/0641/0642**（3 patch = challenge issuer + verifier interface + gtest bundle）
- P2-D 依赖 P2-C 全部完成（49 个 patch 已合入）
- Challenge payload schema 一经冻结不可改（Rust/Swift Guardian 依赖）；新增 challenge type 只能追加，不删旧的

---

## File Structure

**本 sub-plan 交付：**

```
/root/byod-browser/
├── patches/
│   ├── 0640-add-challenge-issuer.patch             # ChallengeIssuer 类
│   ├── 0641-add-challenge-verifier-interface.patch # 验证器接口 + null 实现
│   └── 0642-add-challenge-unit-tests.patch         # gtest bundle
├── build/
│   └── smoke-test-p2d.sh
└── docs/
    └── development/
        ├── p2-d-acceptance.md
        └── ipc-challenge-schema.md                 # 冻结的 challenge schema
```

**Chromium 源码触及区域：**

- 新增 `chrome/browser/exam/ipc/challenge/challenge_types.h`（enum + payload schema）
- 新增 `chrome/browser/exam/ipc/challenge/challenge_issuer.h` + `.cc`
- 新增 `chrome/browser/exam/ipc/challenge/challenge_verifier.h` + `.cc`
- 新增 `chrome/browser/exam/ipc/challenge/challenge_issuer_unittest.cc`
- 修改 `chrome/browser/exam/BUILD.gn` — sources
- 修改 `chrome/test/BUILD.gn` — 加 unittest

**接口冻结契约（P2-E 消费 + P3/P4 Guardian 侧按 schema 实现）：**

```cpp
namespace chrome::exam {

enum class ChallengeType : uint32_t {
  kActiveIme = 1,           // Guardian returns active IME name.
  kRecentProcesses = 2,     // Guardian returns list of recently spawned pids.
  kDisplayList = 3,         // Guardian returns display count + resolutions.
  kAttestationQuote = 4,    // Guardian returns TPM/SE quote for nonce.
};

// Challenge payload JSON schema (frozen):
//   {
//     "type": <ChallengeType uint32>,
//     "nonce": <16-byte base64 random>,
//     "issued_at_monotonic_ms": <uint64>,
//   }
//
// Response payload JSON schema (frozen):
//   {
//     "type": <ChallengeType uint32; must match challenge>,
//     "nonce": <same nonce as challenge>,
//     "responded_at_monotonic_ms": <uint64>,
//     "data": <type-specific object; see ipc-challenge-schema.md>,
//   }

class ChallengeIssuer {
 public:
  // |transport| and |driver| must outlive |this|. |driver| is used to
  // route inbound response frames (they arrive via the transport's global
  // receive callback; ChallengeIssuer registers a delegate on driver).
  ChallengeIssuer(GuardianTransport* transport, IpcKey key);
  ~ChallengeIssuer();

  // Start the 30s repeating timer (spec §5.1.5). Idempotent.
  void Start();
  void Stop();

  // Test hook: manually deliver a response frame (bypass transport).
  void OnResponseFrameForTest(const GuardianFrame& frame);
};

// Verifier abstraction — real cross-checks in P2-E and P5.
class ChallengeVerifier {
 public:
  virtual bool VerifyResponse(ChallengeType type,
                              const std::string& nonce,
                              const base::Value::Dict& response_data) = 0;
  virtual ~ChallengeVerifier() = default;
};

void SetChallengeVerifier(std::unique_ptr<ChallengeVerifier> verifier);
ChallengeVerifier* GetChallengeVerifier();

}  // namespace chrome::exam
```

---

## Challenge Schema 冻结规范

**目的**：Rust (P3) / Swift (P4) Guardian 端按此规范实施 responder，本 sub-plan 结束时 Guardian 端可**独立开发**。

**Challenge payload (Browser → Guardian)**：JSON, UTF-8, no leading BOM

```json
{
  "type": 1,
  "nonce": "aGVsbG8td29ybGQtMTIzNA==",
  "issued_at_monotonic_ms": 1234567890
}
```

**Response payload (Guardian → Browser)**：JSON, UTF-8

```json
{
  "type": 1,
  "nonce": "aGVsbG8td29ybGQtMTIzNA==",
  "responded_at_monotonic_ms": 1234567895,
  "data": { <type-specific> }
}
```

**Type-specific `data` schema**：

| ChallengeType | `data` structure |
|---|---|
| `kActiveIme` (1) | `{ "ime_name": "Microsoft Pinyin", "ime_source_id": "com.microsoft.pinyin" }` |
| `kRecentProcesses` (2) | `{ "processes": [{"pid": 1234, "exe_basename": "notepad.exe", "started_at_monotonic_ms": ...}, ...] }` — list bounded to 100 entries, only new since last challenge |
| `kDisplayList` (3) | `{ "displays": [{"id": 0, "width": 1920, "height": 1080, "is_primary": true}, ...] }` |
| `kAttestationQuote` (4) | `{ "level": "kSoft"\|"kTpm"\|"kSecureEnclave", "fingerprint": "<hex>", "signature": "<hex or empty>" }` |

Guardian MUST respond within 3 seconds; otherwise Browser considers the challenge failed and initiates burn.

冻结文档写入 `docs/development/ipc-challenge-schema.md`（Task 5）。

---

## Task 1: challenge_types.h — 枚举 + 常量 + payload key 名

**Files:**
- Create (in Chromium src): `chrome/browser/exam/ipc/challenge/challenge_types.h`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0640-add-challenge-issuer.patch`（本 patch 后续 Task 2 也会追加内容）
- Create: `/root/byod-browser/build/smoke-test-p2d.sh`

**Interfaces:**
- Consumes: P2-C 全部完成（49 patch 已合入）
- Produces: `ChallengeType` 枚举 + JSON key 名常量供 Task 2 + Guardian 端使用

- [ ] **Step 1: 起始状态自检**

Run:
```bash
cd /opt/chromium/src
git status
git describe --tags
git log --oneline | head -50
```
Expected: `working tree clean`；tag 一致；最近 49 条 commit 是 P1..P2-C 的补丁

若不干净：`echo y | bash /root/byod-browser/build/revert-patches.sh && bash /root/byod-browser/build/apply-patches.sh`

- [ ] **Step 2: 写 smoke-test-p2d.sh 骨架**

Write `/root/byod-browser/build/smoke-test-p2d.sh`:

```bash
#!/usr/bin/env bash
# Behavior smoke tests for P2-D (ChallengeIssuer + ChallengeVerifier).
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
    --no-first-run --no-default-browser-check \
    --disable-gpu --no-sandbox \
    "$@" 2>&1
}

pass() { echo "  ✓ $1"; }
fail() { echo "  ✗ $1"; FAILED=1; }
FAILED=0

# ---------- Task 1: challenge_types.h present ----------
echo "[test] P2-D/task1: challenge types header"
if [ -f /opt/chromium/src/chrome/browser/exam/ipc/challenge/challenge_types.h ]; then
  pass "challenge_types.h present"
else
  fail "challenge_types.h missing"
fi

out="$(run_chrome --headless --dump-dom https://example.com 2>&1 || true)"
if echo "$out" | grep -q "Example Domain"; then
  pass "chrome boots after challenge headers"
else
  fail "chrome broken"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "[fail] one or more P2-D tests failed"; exit 1
fi
echo "[success] all P2-D tests passed"
```

Run: `chmod +x /root/byod-browser/build/smoke-test-p2d.sh`

- [ ] **Step 3: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p2d.sh`

- [ ] **Step 4: 创建 challenge_types.h**

Write `/opt/chromium/src/chrome/browser/exam/ipc/challenge/challenge_types.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P2-D (spec §5.1.5): challenge type enum + JSON key name constants.
// Full JSON payload schema documented in
// docs/development/ipc-challenge-schema.md. Frozen for wire format v1.

#ifndef CHROME_BROWSER_EXAM_IPC_CHALLENGE_CHALLENGE_TYPES_H_
#define CHROME_BROWSER_EXAM_IPC_CHALLENGE_CHALLENGE_TYPES_H_

#include <cstdint>

namespace chrome::exam {

enum class ChallengeType : uint32_t {
  kActiveIme = 1,
  kRecentProcesses = 2,
  kDisplayList = 3,
  kAttestationQuote = 4,
};

namespace challenge_keys {

// Top-level challenge & response payload keys.
inline constexpr char kType[] = "type";
inline constexpr char kNonce[] = "nonce";
inline constexpr char kIssuedAtMonoMs[] = "issued_at_monotonic_ms";
inline constexpr char kRespondedAtMonoMs[] = "responded_at_monotonic_ms";
inline constexpr char kData[] = "data";

// kActiveIme response data keys.
inline constexpr char kImeName[] = "ime_name";
inline constexpr char kImeSourceId[] = "ime_source_id";

// kRecentProcesses response data keys.
inline constexpr char kProcesses[] = "processes";
inline constexpr char kPid[] = "pid";
inline constexpr char kExeBasename[] = "exe_basename";
inline constexpr char kStartedAtMonoMs[] = "started_at_monotonic_ms";

// kDisplayList response data keys.
inline constexpr char kDisplays[] = "displays";
inline constexpr char kDisplayId[] = "id";
inline constexpr char kDisplayWidth[] = "width";
inline constexpr char kDisplayHeight[] = "height";
inline constexpr char kIsPrimary[] = "is_primary";

// kAttestationQuote response data keys.
inline constexpr char kAttLevel[] = "level";
inline constexpr char kAttFingerprint[] = "fingerprint";
inline constexpr char kAttSignature[] = "signature";

// Attestation level string values.
inline constexpr char kLevelSoft[] = "kSoft";
inline constexpr char kLevelTpm[] = "kTpm";
inline constexpr char kLevelSecureEnclave[] = "kSecureEnclave";

}  // namespace challenge_keys

// Timeout for a response after issuance (spec §5.1.5).
inline constexpr int kChallengeTimeoutSeconds = 3;

// Interval between issued challenges (spec §5.1.5).
inline constexpr int kChallengeIntervalSeconds = 30;

// Random nonce length (bytes, before base64 encoding).
inline constexpr int kChallengeNonceLength = 16;

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_IPC_CHALLENGE_CHALLENGE_TYPES_H_
```

- [ ] **Step 5: 修改 BUILD.gn**

在 `sources = [` 加入：

```gn
    "ipc/challenge/challenge_types.h",
```

- [ ] **Step 6: 编译 + smoke**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p2d.sh
```
Expected: 全 pass；`✓ challenge_types.h present`

**说明**：本 task 单独 commit patch 尚早，我们把 Task 1 + Task 2 合并成一个 patch 0640。所以本 task 结束**不导出 patch**，只做本地 commit + 通过 smoke。

- [ ] **Step 7: 本地 Chromium commit（不导出 patch）**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/ipc/challenge/challenge_types.h
git commit -m "byod: add challenge_types.h (P2-D/T1 pre-issuer)"
```

（Task 2 会 amend 或 squash 到最终 patch 0640）

---

## Task 2: ChallengeIssuer — 定时发起 challenge

**Files:**
- Create (in Chromium src): `chrome/browser/exam/ipc/challenge/challenge_issuer.h`
- Create (in Chromium src): `chrome/browser/exam/ipc/challenge/challenge_issuer.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- 导出 patch **0640** 承载 Task 1 + Task 2 全部

**Interfaces:**
- Consumes: Task 1 完成；引用 `GuardianTransport` (P1-D)、`IpcKey` (P2-A)、`FrameCodec/FrameHmac` (P2-B)
- **Produces**（P2-E 消费）：`ChallengeIssuer` 类

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p2d.sh`：

```bash
# ---------- Task 2: ChallengeIssuer header ----------
echo "[test] P2-D/task2: ChallengeIssuer"
if [ -f /opt/chromium/src/chrome/browser/exam/ipc/challenge/challenge_issuer.h ]; then
  pass "challenge_issuer.h present"
else
  fail "challenge_issuer.h missing"
fi
```

- [ ] **Step 2: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p2d.sh`

- [ ] **Step 3: 创建 challenge_issuer.h**

Write `/opt/chromium/src/chrome/browser/exam/ipc/challenge/challenge_issuer.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P2-D Task 2 (spec §5.1.5): periodic challenge issuer.
//
// Every 30s (kChallengeIntervalSeconds) picks a random ChallengeType,
// generates a 16-byte nonce, builds a JSON payload per the frozen schema
// (ipc-challenge-schema.md), and sends it through GuardianTransport.
//
// Starts a 3s timeout for each pending challenge; timeout with no valid
// response triggers ExamSession::TriggerBurn("challenge_timeout").

#ifndef CHROME_BROWSER_EXAM_IPC_CHALLENGE_CHALLENGE_ISSUER_H_
#define CHROME_BROWSER_EXAM_IPC_CHALLENGE_CHALLENGE_ISSUER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/browser/exam/ipc/challenge/challenge_types.h"
#include "chrome/browser/exam/ipc/guardian_transport.h"
#include "chrome/browser/exam/ipc/handshake/key_channel.h"  // IpcKey
#include "chrome/browser/exam/ipc/wire/sequence_guard.h"

namespace chrome::exam {

class ChallengeIssuer {
 public:
  ChallengeIssuer(GuardianTransport* transport, IpcKey key);
  ~ChallengeIssuer();

  ChallengeIssuer(const ChallengeIssuer&) = delete;
  ChallengeIssuer& operator=(const ChallengeIssuer&) = delete;

  // Start the 30s repeating timer. Idempotent.
  void Start();

  // Stop and clear all pending challenges. Idempotent.
  void Stop();

  // Test hook: deliver a response frame directly.
  void OnResponseFrameForTest(const GuardianFrame& frame);

  // Observability.
  size_t pending_count() const { return pending_.size(); }
  uint64_t total_issued() const { return total_issued_; }
  uint64_t total_responded() const { return total_responded_; }
  uint64_t total_timed_out() const { return total_timed_out_; }

 private:
  struct Pending {
    ChallengeType type;
    std::string nonce_base64;
    int64_t issued_at_monotonic_ms;
    // Timeout timer for THIS challenge (owned).
    std::unique_ptr<base::OneShotTimer> timeout_timer;
  };

  // Called by |issue_timer_| every 30s.
  void IssueOne();

  // Called by a Pending's timeout timer.
  void OnPendingTimeout(uint64_t seq);

  // Dispatch an inbound response frame: verify HMAC, parse JSON,
  // match against pending, cancel its timeout timer, notify verifier.
  void OnResponseFrame(const GuardianFrame& frame);

  raw_ptr<GuardianTransport> transport_;
  IpcKey key_;
  base::RepeatingTimer issue_timer_;
  std::map<uint64_t /*seq*/, Pending> pending_;
  uint64_t next_outbound_seq_ = 0;

  uint64_t total_issued_ = 0;
  uint64_t total_responded_ = 0;
  uint64_t total_timed_out_ = 0;
  bool started_ = false;

  base::WeakPtrFactory<ChallengeIssuer> weak_factory_{this};
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_IPC_CHALLENGE_CHALLENGE_ISSUER_H_
```

- [ ] **Step 4: 创建 challenge_issuer.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/challenge/challenge_issuer.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/ipc/challenge/challenge_issuer.h"

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/exam/ipc/challenge/challenge_verifier.h"
#include "chrome/browser/exam/ipc/wire/frame_codec.h"
#include "chrome/browser/exam/ipc/wire/frame_hmac.h"
#include "chrome/browser/exam/session/exam_session.h"

namespace chrome::exam {

namespace {

// Cycle through all challenge types deterministically for now. Random
// selection is possible via base::RandInt but deterministic rotation
// gives coverage across all types within 2 minutes, easier to reason
// about in tests. Random selection can be re-enabled by flipping this
// flag; both are equally hard to predict from Guardian's viewpoint.
constexpr bool kUseRandomSelection = true;

ChallengeType PickChallengeType(uint64_t rotation_counter) {
  if (kUseRandomSelection) {
    // 4 types → uniform random in [1, 4].
    const int r = base::RandInt(1, 4);
    return static_cast<ChallengeType>(r);
  }
  // Deterministic rotation.
  const uint32_t types_count = 4;
  const uint32_t idx = (rotation_counter % types_count) + 1;
  return static_cast<ChallengeType>(idx);
}

std::string GenerateNonceBase64() {
  std::string bytes(kChallengeNonceLength, '\0');
  base::RandBytes(reinterpret_cast<uint8_t*>(bytes.data()), bytes.size());
  return base::Base64Encode(bytes);
}

int64_t MonotonicMillisNow() {
  return (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds();
}

}  // namespace

ChallengeIssuer::ChallengeIssuer(GuardianTransport* transport, IpcKey key)
    : transport_(transport), key_(key) {}

ChallengeIssuer::~ChallengeIssuer() {
  key_.Clear();
}

void ChallengeIssuer::Start() {
  if (started_) return;
  started_ = true;
  issue_timer_.Start(
      FROM_HERE,
      base::Seconds(kChallengeIntervalSeconds),
      base::BindRepeating(&ChallengeIssuer::IssueOne,
                          weak_factory_.GetWeakPtr()));
  LOG(INFO) << "[byod] ChallengeIssuer started";
}

void ChallengeIssuer::Stop() {
  if (!started_) return;
  started_ = false;
  issue_timer_.Stop();
  // Cancel all pending timeouts.
  for (auto& [seq, pending] : pending_) {
    if (pending.timeout_timer) pending.timeout_timer->Stop();
  }
  pending_.clear();
  LOG(INFO) << "[byod] ChallengeIssuer stopped";
}

void ChallengeIssuer::OnResponseFrameForTest(const GuardianFrame& frame) {
  OnResponseFrame(frame);
}

void ChallengeIssuer::IssueOne() {
  if (!transport_ || !transport_->IsConnected()) return;

  const uint64_t seq = ++next_outbound_seq_;
  ChallengeType type = PickChallengeType(seq);
  std::string nonce = GenerateNonceBase64();
  int64_t issued_ms = MonotonicMillisNow();

  // Build JSON payload.
  base::Value::Dict dict;
  dict.Set(challenge_keys::kType, static_cast<int>(type));
  dict.Set(challenge_keys::kNonce, nonce);
  dict.Set(challenge_keys::kIssuedAtMonoMs, static_cast<double>(issued_ms));
  std::string json;
  if (!base::JSONWriter::Write(dict, &json)) {
    LOG(ERROR) << "[byod] ChallengeIssuer: JSON serialize failed";
    return;
  }

  GuardianFrame frame;
  frame.seq = seq;
  frame.type = MessageType::kChallenge;
  frame.payload.assign(json.begin(), json.end());
  auto enc = frame_codec::EncodeUnsigned(frame);
  if (!enc) {
    LOG(ERROR) << "[byod] ChallengeIssuer: encode failed";
    return;
  }
  frame.hmac = frame_hmac::Compute(key_, *enc);

  if (!transport_->Send(frame)) {
    LOG(WARNING) << "[byod] ChallengeIssuer: transport Send failed seq=" << seq;
    return;
  }

  // Record pending + start 3s timeout.
  Pending pending;
  pending.type = type;
  pending.nonce_base64 = nonce;
  pending.issued_at_monotonic_ms = issued_ms;
  pending.timeout_timer = std::make_unique<base::OneShotTimer>();
  pending.timeout_timer->Start(
      FROM_HERE,
      base::Seconds(kChallengeTimeoutSeconds),
      base::BindOnce(&ChallengeIssuer::OnPendingTimeout,
                     weak_factory_.GetWeakPtr(), seq));
  pending_.emplace(seq, std::move(pending));
  ++total_issued_;
}

void ChallengeIssuer::OnPendingTimeout(uint64_t seq) {
  auto it = pending_.find(seq);
  if (it == pending_.end()) return;  // Already responded.
  LOG(ERROR) << "[byod] ChallengeIssuer: challenge seq=" << seq
             << " timed out — triggering burn";
  pending_.erase(it);
  ++total_timed_out_;
  ExamSession::Get().TriggerBurn("challenge_timeout");
  Stop();
}

void ChallengeIssuer::OnResponseFrame(const GuardianFrame& frame) {
  if (frame.type != MessageType::kChallengeResponse) return;

  // Verify HMAC first.
  if (!frame_hmac::Verify(key_, frame)) {
    LOG(WARNING) << "[byod] ChallengeIssuer: response HMAC bad, seq="
                 << frame.seq;
    return;
  }

  // Parse JSON.
  const std::string payload_str(frame.payload.begin(), frame.payload.end());
  auto parsed = base::JSONReader::Read(payload_str);
  if (!parsed || !parsed->is_dict()) {
    LOG(WARNING) << "[byod] ChallengeIssuer: response JSON parse fail";
    return;
  }
  const base::Value::Dict& d = parsed->GetDict();

  const std::string* nonce = d.FindString(challenge_keys::kNonce);
  const std::optional<int> type_int = d.FindInt(challenge_keys::kType);
  const base::Value::Dict* data = d.FindDict(challenge_keys::kData);
  if (!nonce || !type_int || !data) {
    LOG(WARNING) << "[byod] ChallengeIssuer: response missing fields";
    return;
  }

  // Match against pending. We assume response.seq matches challenge.seq
  // (Guardian must echo). If Guardian uses its own outbound seq, we'd
  // match by nonce instead — spec §5.1.5 says seq must be echoed.
  auto it = pending_.find(frame.seq);
  if (it == pending_.end()) {
    LOG(WARNING) << "[byod] ChallengeIssuer: response for unknown seq="
                 << frame.seq;
    return;
  }
  const Pending& p = it->second;
  if (p.nonce_base64 != *nonce ||
      static_cast<int>(p.type) != *type_int) {
    LOG(WARNING) << "[byod] ChallengeIssuer: response nonce/type mismatch";
    return;
  }

  // Cancel timeout.
  if (p.timeout_timer) p.timeout_timer->Stop();

  // Delegate to verifier (P2-E / P5 provides real impl).
  if (auto* verifier = GetChallengeVerifier()) {
    if (!verifier->VerifyResponse(p.type, p.nonce_base64, *data)) {
      LOG(ERROR) << "[byod] ChallengeIssuer: verifier REJECTED response — burn";
      pending_.erase(it);
      ExamSession::Get().TriggerBurn("challenge_rejected");
      Stop();
      return;
    }
  }

  ++total_responded_;
  pending_.erase(it);
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "ipc/challenge/challenge_issuer.cc",
    "ipc/challenge/challenge_issuer.h",
```

- [ ] **Step 6: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**常见报错**：
- `base::JSONWriter::Write` 签名变化：新版 API 接受 `base::ValueView`；改成 `base::JSONWriter::Write(base::Value(std::move(dict)), &json)`
- `base::Base64Encode` 返回 `std::string` (老 API) vs `void` (新版需要 out 参数)：按报错调整；新版可能是 `base::Base64Encode(bytes, &out)`
- `ChallengeVerifier` 尚未定义：Task 3 才创建；本 Task 6 编译会失败**依赖前置**。修法：**先创建 `challenge_verifier.h` 空 stub**，让 Task 2 能编译；Task 3 里再补 verifier 完整实现

**紧急修法**（如果 Task 2 编译报"ChallengeVerifier undefined"）：先跳到 Task 3 Step 3 创建 `challenge_verifier.h` 空 stub（只声明类 + Get/Set + null 实现），再回到本 Task 6 编译。

- [ ] **Step 7: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p2d.sh
```

- [ ] **Step 8: 合并 Task 1 + Task 2 提交，导出 patch 0640**

Run:
```bash
cd /opt/chromium/src
# Task 1 已有独立 commit，Task 2 追加后 amend or squash
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/ipc/challenge/challenge_issuer.h \
        chrome/browser/exam/ipc/challenge/challenge_issuer.cc
# Squash into Task 1's commit (safer than amend if Task 1 is HEAD^)
git commit -m "byod: add ChallengeIssuer (issue + timeout + response match) (P2-D/T2)

P2-D Task 2 (spec §5.1.5). 30s repeating timer picks random ChallengeType,
generates 16-byte nonce, sends via GuardianTransport with HMAC. Each
pending challenge has a 3s OneShotTimer; timeout triggers
ExamSession::TriggerBurn('challenge_timeout'). Inbound response matched
by seq, verified by installed ChallengeVerifier (Task 3)."

# Squash the two commits into one before exporting.
git reset --soft HEAD~2
git commit -m "byod: add ChallengeIssuer + challenge_types.h (P2-D/T1+T2)

P2-D Tasks 1-2 (spec §5.1.5). Frozen ChallengeType enum + JSON key names +
ChallengeIssuer class (30s timer, 3s pending timeout, HMAC-signed frames,
JSON payload per ipc-challenge-schema.md)."

git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0640-add-challenge-issuer.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p2d.sh

cd /root/byod-browser
git add patches/0640-add-challenge-issuer.patch build/smoke-test-p2d.sh
git commit -m "patch: 0640 ChallengeIssuer + challenge_types (P2-D/T1+T2)

Est. LOC in Chromium: ~280 (issuer 200 + types 60 + BUILD.gn)."
```

---

## Task 3: ChallengeVerifier 接口 + null 实现

**Files:**
- Create (in Chromium src): `chrome/browser/exam/ipc/challenge/challenge_verifier.h`
- Create (in Chromium src): `chrome/browser/exam/ipc/challenge/challenge_verifier.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0641-add-challenge-verifier-interface.patch`

**Interfaces:**
- Consumes: Task 2 完成
- **Produces**（P2-E + P5 消费）：`ChallengeVerifier` 抽象接口 + `NullChallengeVerifier` 默认（accept-all）+ Set/Get 全局

**说明**：**真正的 challenge 校验**（比如 Browser 自己也采集显示器数与 Guardian 报的对账、TPM quote 签名验证）由 P2-E（集成阶段）和 P5（策略引擎）实现。本 Task 只冻结接口 + 提供 accept-all 默认。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p2d.sh`：

```bash
# ---------- Task 3: ChallengeVerifier interface ----------
echo "[test] P2-D/task3: ChallengeVerifier"
if [ -f /opt/chromium/src/chrome/browser/exam/ipc/challenge/challenge_verifier.h ]; then
  pass "challenge_verifier.h present"
else
  fail "challenge_verifier.h missing"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p2d.sh`

- [ ] **Step 3: 创建 challenge_verifier.h**

Write `/opt/chromium/src/chrome/browser/exam/ipc/challenge/challenge_verifier.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P2-D Task 3 (spec §5.1.5): abstract verifier for challenge responses.
// The default installed verifier is NullChallengeVerifier which accepts all
// well-formed responses. P2-E and P5 install a real verifier that:
//   - kActiveIme: cross-checks Guardian's report against Browser-side IME
//     policy (spec §4.2.3).
//   - kRecentProcesses: independently queries process list and compares.
//   - kDisplayList: independently queries via display::Screen and compares.
//   - kAttestationQuote: verifies TPM/SE signature against enrolled key.

#ifndef CHROME_BROWSER_EXAM_IPC_CHALLENGE_CHALLENGE_VERIFIER_H_
#define CHROME_BROWSER_EXAM_IPC_CHALLENGE_CHALLENGE_VERIFIER_H_

#include <memory>
#include <string>

#include "base/values.h"
#include "chrome/browser/exam/ipc/challenge/challenge_types.h"

namespace chrome::exam {

class ChallengeVerifier {
 public:
  // Return true iff the response is acceptable. False triggers burn via
  // ChallengeIssuer.
  virtual bool VerifyResponse(ChallengeType type,
                              const std::string& nonce_base64,
                              const base::Value::Dict& data) = 0;
  virtual ~ChallengeVerifier() = default;
};

// Install a verifier. Passing nullptr installs the default null verifier
// (accept-all).
void SetChallengeVerifier(std::unique_ptr<ChallengeVerifier> verifier);
ChallengeVerifier* GetChallengeVerifier();

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_IPC_CHALLENGE_CHALLENGE_VERIFIER_H_
```

- [ ] **Step 4: 创建 challenge_verifier.cc（含 NullChallengeVerifier）**

Write `/opt/chromium/src/chrome/browser/exam/ipc/challenge/challenge_verifier.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/ipc/challenge/challenge_verifier.h"

#include "base/no_destructor.h"

namespace chrome::exam {

namespace {

// P1-D pattern: NoDestructor singleton holder, no lock (Set called from
// browser main once at startup).
class NullChallengeVerifier : public ChallengeVerifier {
 public:
  bool VerifyResponse(ChallengeType /*type*/,
                      const std::string& /*nonce_base64*/,
                      const base::Value::Dict& /*data*/) override {
    return true;
  }
};

std::unique_ptr<ChallengeVerifier>& MutableVerifier() {
  static base::NoDestructor<std::unique_ptr<ChallengeVerifier>> holder(
      std::make_unique<NullChallengeVerifier>());
  return *holder;
}

}  // namespace

void SetChallengeVerifier(std::unique_ptr<ChallengeVerifier> verifier) {
  MutableVerifier() = verifier ? std::move(verifier)
                               : std::make_unique<NullChallengeVerifier>();
}

ChallengeVerifier* GetChallengeVerifier() {
  return MutableVerifier().get();
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "ipc/challenge/challenge_verifier.cc",
    "ipc/challenge/challenge_verifier.h",
```

- [ ] **Step 6: 编译 + smoke**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p2d.sh
```

- [ ] **Step 7: 导出 patch + revert/re-apply 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/ipc/challenge/challenge_verifier.h \
        chrome/browser/exam/ipc/challenge/challenge_verifier.cc
git commit -m "byod: add ChallengeVerifier interface + NullVerifier (P2-D/T3)

P2-D Task 3 (spec §5.1.5). Abstract verifier called by ChallengeIssuer
after HMAC + JSON schema check. Default NullChallengeVerifier accepts
all responses. Real implementations installed by P2-E integration and
P5 policy engine (cross-check IME/processes/displays independently,
verify TPM/SE signatures)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0641-add-challenge-verifier-interface.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p2d.sh

cd /root/byod-browser
git add patches/0641-add-challenge-verifier-interface.patch build/smoke-test-p2d.sh
git commit -m "patch: 0641 ChallengeVerifier interface (P2-D/T3)

Est. LOC in Chromium: ~70."
```

---

## Task 4: gtest 单元测试 — ChallengeIssuer 行为

**Files:**
- Create (in Chromium src): `chrome/browser/exam/ipc/challenge/challenge_issuer_unittest.cc`
- Modify (in Chromium src): `chrome/test/BUILD.gn`
- Create: `/root/byod-browser/patches/0642-add-challenge-unit-tests.patch`

**Interfaces:**
- Consumes: Task 3 完成
- Produces: gtest 覆盖 5 个场景：初始 0 issued、Start 后 30s 触发一次 issue、pending 3s 未响应→burn、有效响应清除 pending、rejected verifier→burn

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p2d.sh`：

```bash
# ---------- Task 4: unit tests present ----------
echo "[test] P2-D/task4: ChallengeIssuer unit tests"
if [ -f /opt/chromium/src/chrome/browser/exam/ipc/challenge/challenge_issuer_unittest.cc ]; then
  pass "challenge_issuer_unittest.cc present"
else
  fail "challenge_issuer_unittest.cc missing"
fi
```

- [ ] **Step 2: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p2d.sh`

- [ ] **Step 3: 创建 challenge_issuer_unittest.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/challenge/challenge_issuer_unittest.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P2-D Task 4: ChallengeIssuer unit tests.

#include "chrome/browser/exam/ipc/challenge/challenge_issuer.h"

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "chrome/browser/exam/ipc/challenge/challenge_verifier.h"
#include "chrome/browser/exam/ipc/wire/frame_codec.h"
#include "chrome/browser/exam/ipc/wire/frame_hmac.h"
#include "chrome/browser/exam/session/exam_session.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chrome::exam {

namespace {

class MockTransport : public GuardianTransport {
 public:
  bool Send(const GuardianFrame& frame) override {
    sent_frames.push_back(frame);
    return connected;
  }
  void SetOnFrameCallback(OnFrameCallback cb) override { on_frame = cb; }
  bool IsConnected() const override { return connected; }
  bool connected = true;
  std::vector<GuardianFrame> sent_frames;
  OnFrameCallback on_frame;
};

class TrackingVerifier : public ChallengeVerifier {
 public:
  bool VerifyResponse(ChallengeType type,
                      const std::string& /*nonce*/,
                      const base::Value::Dict& /*data*/) override {
    ++calls;
    last_type = type;
    return accept;
  }
  int calls = 0;
  ChallengeType last_type = ChallengeType::kActiveIme;
  bool accept = true;
};

IpcKey MakeKey() {
  IpcKey k;
  for (size_t i = 0; i < k.bytes.size(); ++i) {
    k.bytes[i] = static_cast<uint8_t>(0x22 * (i + 1));
  }
  return k;
}

// Build a Guardian response frame that matches the payload the issuer
// just sent (extract seq + nonce + type from the sent frame).
GuardianFrame BuildValidResponse(const IpcKey& key,
                                 const GuardianFrame& challenge) {
  const std::string payload_str(challenge.payload.begin(),
                                challenge.payload.end());
  auto parsed = base::JSONReader::Read(payload_str);
  CHECK(parsed && parsed->is_dict());
  const auto& d = parsed->GetDict();
  const std::string* nonce = d.FindString(challenge_keys::kNonce);
  const std::optional<int> type_int = d.FindInt(challenge_keys::kType);
  CHECK(nonce);
  CHECK(type_int);

  base::Value::Dict resp;
  resp.Set(challenge_keys::kType, *type_int);
  resp.Set(challenge_keys::kNonce, *nonce);
  resp.Set(challenge_keys::kRespondedAtMonoMs, 0.0);
  resp.Set(challenge_keys::kData, base::Value::Dict());  // empty; verifier accepts

  std::string json;
  CHECK(base::JSONWriter::Write(resp, &json));

  GuardianFrame f;
  f.seq = challenge.seq;
  f.type = MessageType::kChallengeResponse;
  f.payload.assign(json.begin(), json.end());
  auto enc = frame_codec::EncodeUnsigned(f);
  CHECK(enc.has_value());
  f.hmac = frame_hmac::Compute(key, *enc);
  return f;
}

class ChallengeIssuerTest : public ::testing::Test {
 protected:
  base::test::TaskEnvironment env_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  MockTransport transport_;
  IpcKey key_ = MakeKey();

  void SetUp() override {
    if (ExamSession::Get().state() == SessionState::kBurning ||
        ExamSession::Get().state() == SessionState::kEnded) {
      GTEST_SKIP() << "ExamSession terminal from prior test — singleton leak";
    }
    ExamSession::Get().StartExam("test-session");
    SetChallengeVerifier(nullptr);  // reset to null (accept-all)
  }
};

TEST_F(ChallengeIssuerTest, InitialCountersZero) {
  ChallengeIssuer issuer(&transport_, key_);
  EXPECT_EQ(0u, issuer.total_issued());
  EXPECT_EQ(0u, issuer.pending_count());
}

TEST_F(ChallengeIssuerTest, StartIssuesAfter30s) {
  ChallengeIssuer issuer(&transport_, key_);
  issuer.Start();

  env_.FastForwardBy(base::Seconds(29));
  EXPECT_EQ(0u, transport_.sent_frames.size());

  env_.FastForwardBy(base::Seconds(2));  // total 31s, 1 issued
  EXPECT_EQ(1u, transport_.sent_frames.size());
  EXPECT_EQ(1u, issuer.total_issued());
  EXPECT_EQ(1u, issuer.pending_count());
  EXPECT_EQ(MessageType::kChallenge, transport_.sent_frames[0].type);
}

TEST_F(ChallengeIssuerTest, NoResponseWithin3sTriggersBurn) {
  ChallengeIssuer issuer(&transport_, key_);
  issuer.Start();
  env_.FastForwardBy(base::Seconds(30));  // issue 1
  ASSERT_EQ(1u, issuer.pending_count());

  env_.FastForwardBy(base::Seconds(4));  // past 3s timeout
  EXPECT_EQ(1u, issuer.total_timed_out());
  EXPECT_EQ(SessionState::kBurning, ExamSession::Get().state());
  EXPECT_EQ("challenge_timeout", ExamSession::Get().burn_reason());
}

TEST_F(ChallengeIssuerTest, ValidResponseClearsPending) {
  ChallengeIssuer issuer(&transport_, key_);
  issuer.Start();
  env_.FastForwardBy(base::Seconds(30));
  ASSERT_EQ(1u, transport_.sent_frames.size());
  auto& sent = transport_.sent_frames[0];

  auto resp = BuildValidResponse(key_, sent);
  issuer.OnResponseFrameForTest(resp);

  EXPECT_EQ(0u, issuer.pending_count());
  EXPECT_EQ(1u, issuer.total_responded());
  EXPECT_EQ(SessionState::kActive, ExamSession::Get().state());
}

TEST_F(ChallengeIssuerTest, RejectedResponseTriggersBurn) {
  auto tracker_owned = std::make_unique<TrackingVerifier>();
  auto* tracker = tracker_owned.get();
  tracker->accept = false;
  SetChallengeVerifier(std::move(tracker_owned));

  ChallengeIssuer issuer(&transport_, key_);
  issuer.Start();
  env_.FastForwardBy(base::Seconds(30));
  ASSERT_EQ(1u, transport_.sent_frames.size());
  auto& sent = transport_.sent_frames[0];

  auto resp = BuildValidResponse(key_, sent);
  issuer.OnResponseFrameForTest(resp);

  EXPECT_EQ(1, tracker->calls);
  EXPECT_EQ(SessionState::kBurning, ExamSession::Get().state());
  EXPECT_EQ("challenge_rejected", ExamSession::Get().burn_reason());

  // Reset for next test.
  SetChallengeVerifier(nullptr);
}

TEST_F(ChallengeIssuerTest, StopClearsPending) {
  ChallengeIssuer issuer(&transport_, key_);
  issuer.Start();
  env_.FastForwardBy(base::Seconds(30));
  ASSERT_EQ(1u, issuer.pending_count());

  issuer.Stop();
  EXPECT_EQ(0u, issuer.pending_count());

  env_.FastForwardBy(base::Seconds(10));  // Would have timed out; verify no burn.
  EXPECT_NE(SessionState::kBurning, ExamSession::Get().state());
}

}  // namespace

}  // namespace chrome::exam
```

- [ ] **Step 4: 加进 chrome/test/BUILD.gn**

在 `unit_tests` target 加：

```gn
    "../browser/exam/ipc/challenge/challenge_issuer_unittest.cc",
```

- [ ] **Step 5: 编译 + 跑 gtest**

Run:
```bash
autoninja -C /opt/chromium/src/out/linux unit_tests
/opt/chromium/src/out/linux/unit_tests \
  --gtest_filter="ChallengeIssuerTest.*" 2>&1 | tail -20
```
Expected: 6 个 test 全 PASS（或第一个后续 SKIP 因 singleton 污染）

- [ ] **Step 6: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p2d.sh
```

- [ ] **Step 7: 导出 patch + verify + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/ipc/challenge/challenge_issuer_unittest.cc \
        chrome/test/BUILD.gn
git commit -m "byod: add ChallengeIssuer gtest suite (P2-D/T4)

P2-D Task 4. 6 tests using MOCK_TIME: initial counters, timer cadence,
timeout triggers burn, valid response clears pending, rejected response
triggers burn, Stop clears pending. Handles singleton pollution via
GTEST_SKIP."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0642-add-challenge-unit-tests.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
autoninja -C /opt/chromium/src/out/linux unit_tests
/opt/chromium/src/out/linux/unit_tests --gtest_filter="ChallengeIssuerTest.*" 2>&1 | tail -5

cd /root/byod-browser
git add patches/0642-add-challenge-unit-tests.patch build/smoke-test-p2d.sh
git commit -m "patch: 0642 ChallengeIssuer unit tests (P2-D/T4)

Est. LOC in Chromium: ~180."
```

---

## Task 5: 冻结 challenge schema 文档

**Files:**
- Create: `/root/byod-browser/docs/development/ipc-challenge-schema.md`

- [ ] **Step 1: 写规范文档**

Write `/root/byod-browser/docs/development/ipc-challenge-schema.md`:

```markdown
# BYOD IPC Challenge / Response Schema (v1)

**Frozen at**: 2026-07-12 (P2-D Task 5)
**Companion**: [`ipc-wire-format.md`](ipc-wire-format.md) covers the frame
layout; this document covers the JSON payloads for `kChallenge` and
`kChallengeResponse` message types.

## Cadence

- Browser issues 1 challenge every **30 seconds** (`kChallengeIntervalSeconds`).
- Guardian MUST respond within **3 seconds** (`kChallengeTimeoutSeconds`).
- Failure to respond within timeout → Browser burns the session
  (`ExamSession::TriggerBurn("challenge_timeout")`).

## Challenge Payload (Browser → Guardian)

Sent as `MessageType::kChallenge` frame. Payload is UTF-8 JSON:

```json
{
  "type": <uint32>,
  "nonce": "<base64 of 16 random bytes>",
  "issued_at_monotonic_ms": <uint64>
}
```

- `type`: one of the ChallengeType values below.
- `nonce`: unique per challenge; Guardian MUST echo verbatim.
- `issued_at_monotonic_ms`: monotonic clock at issuance (for latency logging).

## Response Payload (Guardian → Browser)

Sent as `MessageType::kChallengeResponse` frame with the **same `seq`** as
the challenge it replies to. Payload is UTF-8 JSON:

```json
{
  "type": <uint32>,      // MUST match challenge.type
  "nonce": "<base64>",   // MUST match challenge.nonce
  "responded_at_monotonic_ms": <uint64>,
  "data": { <type-specific object> }
}
```

## ChallengeType Enum

| Value | Symbol              | Data Schema | Notes |
|-------|---------------------|-------------|-------|
| 1     | kActiveIme          | see below   | Current active IME on the desktop |
| 2     | kRecentProcesses    | see below   | Processes created since last challenge |
| 3     | kDisplayList        | see below   | All displays attached |
| 4     | kAttestationQuote   | see below   | TPM/SE signature over the nonce |

Future values MUST be added at the end. Do not renumber.

## Data Schemas per Type

### kActiveIme (type=1)

```json
{
  "ime_name": "Microsoft Pinyin",
  "ime_source_id": "com.microsoft.pinyin"
}
```

- `ime_name`: display name (human-readable).
- `ime_source_id`: OS-provided identifier
  (Windows: registry KLID or ProgID; macOS: TIS input source ID).

**Guardian implementation**:
- Windows: `GetKeyboardLayout` + `ImmGetIMEFileName` +
  `GetLocaleInfoEx(LOCALE_NAME_INVARIANT, ...)`.
- macOS: `TISCopyCurrentKeyboardInputSource` + `TISGetInputSourceProperty(
  kTISPropertyInputSourceID)` + `... kTISPropertyLocalizedName`.

### kRecentProcesses (type=2)

```json
{
  "processes": [
    {
      "pid": 1234,
      "exe_basename": "notepad.exe",
      "started_at_monotonic_ms": 1234500000
    },
    ...
  ]
}
```

- List MUST be bounded to at most **100 entries**.
- Only processes started **since the last challenge** (Guardian tracks
  its own high-water mark).
- `exe_basename`: filename only (no path); Guardian may include full path
  only if basename is ambiguous.

**Guardian implementation**:
- Windows: ETW `Microsoft-Windows-Kernel-Process` provider event buffer
  (P3 will describe details).
- macOS: Endpoint Security Framework process creation events (P4).

### kDisplayList (type=3)

```json
{
  "displays": [
    {"id": 0, "width": 1920, "height": 1080, "is_primary": true},
    {"id": 1, "width": 2560, "height": 1440, "is_primary": false}
  ]
}
```

- `id`: OS display identifier (stable within session).
- `width`/`height`: in pixels at native scale.

**Guardian implementation**:
- Windows: `EnumDisplayDevices` + `EnumDisplaySettings` for each device.
- macOS: `CGGetActiveDisplayList` + `CGDisplayBounds` for each.

### kAttestationQuote (type=4)

```json
{
  "level": "kSoft" | "kTpm" | "kSecureEnclave",
  "fingerprint": "<hex-encoded stable device id>",
  "signature": "<hex-encoded EC/RSA signature over challenge.nonce>"
}
```

- `level`: matches `AttestationLevel` from P1-E.
- `fingerprint`: SHA-256 hex (soft) or TPM/SE-bound identifier.
- `signature`: **empty string if level=kSoft**; otherwise hex-encoded
  signature over the raw nonce bytes (16 bytes, NOT base64-decoded).

**Guardian implementation**:
- Windows TPM 2.0: `Tbsi_*` + `NCryptSignHash` with attestation key.
- macOS Secure Enclave: `SecKeyCreateSignature`.

**Browser verification** (P2-E or P5):
- Independently compute fingerprint via P1-E `HardwareAttestor`.
- Verify signature against enrolled public key.

## Cross-check Verification

The **NullChallengeVerifier** (P2-D default) accepts all well-formed
responses. Real verifiers (P2-E integration + P5 policy engine) SHOULD:

1. **kActiveIme**: check `ime_source_id` is in the IME whitelist (spec §4.2.3).
2. **kRecentProcesses**: check no entries match the process blacklist
   (spec §4.2.1); optionally cross-check with Browser's own polled snapshot.
3. **kDisplayList**: cross-check with `display::Screen::GetAllDisplays()`;
   if mismatch, this is a strong signal Guardian is mocked / bypassed.
4. **kAttestationQuote**: verify hardware signature.

Any check failure → verifier returns false → `TriggerBurn("challenge_rejected")`.

## Version Bump Procedure

If we need to add fields:

1. Additive-only fields are safe (both sides ignore unknown keys).
2. Removing / renaming fields → bump wire version (spec §5.1.3);
   this document version bumps in lockstep.

## History

- **v1** (2026-07-12): initial schema. Frozen for BYOD Exam Browser v1.0.
```

- [ ] **Step 2: Commit**

Run:
```bash
cd /root/byod-browser
git add docs/development/ipc-challenge-schema.md
git commit -m "docs: freeze IPC challenge / response schema v1 for P3/P4

P2-D Task 5. Full JSON schema for 4 ChallengeType values with Guardian
implementation notes. Rust (P3) and Swift (P4) can implement responders
without reading Chromium C++."
```

---

## Task 6: P2-D 交付验收清单

**Files:**
- Create: `/root/byod-browser/docs/development/p2-d-acceptance.md`

- [ ] **Step 1: 统计 patch 总量**

Run:
```bash
ls /root/byod-browser/patches/*.patch | wc -l
```
Expected: **52**（P1 34 + P2-A 7 + P2-B 4 + P2-C 4 + P2-D 3）

- [ ] **Step 2: 全量 revert + apply + 编译 + smoke + gtest**

Run:
```bash
echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
for s in smoke-test smoke-test-p1a smoke-test-p1b smoke-test-p1c \
         smoke-test-p1d smoke-test-p1e \
         smoke-test-p2a smoke-test-p2b smoke-test-p2c smoke-test-p2d; do
  bash "/root/byod-browser/build/$s.sh"
done
autoninja -C /opt/chromium/src/out/linux unit_tests
/opt/chromium/src/out/linux/unit_tests \
  --gtest_filter="IpcKeyTest.*:KeyChannelTest.*:PeerAttestorTest.*:FrameCodecTest.*:FrameHmacTest.*:SequenceGuardTest.*:HeartbeatDriverTest.*:ChallengeIssuerTest.*" \
  2>&1 | tail -10
```
Expected: 全部成功

- [ ] **Step 3: 写 acceptance checklist**

Write `/root/byod-browser/docs/development/p2-d-acceptance.md`:

```markdown
# P2-D 交付验收 Checklist

## 补丁类

- [ ] `patches/0640-add-challenge-issuer.patch` 存在
- [ ] `patches/0641-add-challenge-verifier-interface.patch` 存在
- [ ] `patches/0642-add-challenge-unit-tests.patch` 存在

## 编译类

- [ ] `bash build/revert-patches.sh && bash build/apply-patches.sh` 无冲突
- [ ] `bash build/build-linux.sh chrome` 成功
- [ ] `autoninja -C out/linux unit_tests` 成功

## 行为类

- [ ] `smoke-test.sh` + `smoke-test-p1a..e.sh` + `smoke-test-p2a..d.sh` 全 pass
- [ ] `unit_tests --gtest_filter="ChallengeIssuerTest.*"`：6 个 test PASS 或部分 SKIP 因 singleton
  - `InitialCountersZero` PASS
  - `StartIssuesAfter30s` PASS
  - `NoResponseWithin3sTriggersBurn` PASS 或 SKIP
  - `ValidResponseClearsPending` PASS 或 SKIP
  - `RejectedResponseTriggersBurn` PASS 或 SKIP
  - `StopClearsPending` PASS 或 SKIP

## 接口冻结类（供 P2-E/P3/P4 消费）

- [ ] `chrome::exam::ChallengeType` 枚举已 export（4 值：kActiveIme/kRecentProcesses/kDisplayList/kAttestationQuote）
- [ ] `chrome::exam::challenge_keys::*` 所有 JSON key 常量已 export
- [ ] `kChallengeIntervalSeconds` = 30、`kChallengeTimeoutSeconds` = 3、`kChallengeNonceLength` = 16 常量已 export
- [ ] `chrome::exam::ChallengeIssuer` 类（Start/Stop/OnResponseFrameForTest/pending_count/total_*）已 export
- [ ] `chrome::exam::ChallengeVerifier` 抽象 + `SetChallengeVerifier` / `GetChallengeVerifier` 已 export
- [ ] `docs/development/ipc-challenge-schema.md` 冻结（Rust/Swift 可直接实施 responder）

## 补丁行数 / 预算类

- [ ] 累计补丁：52（P1 34 + P2-A 7 + P2-B 4 + P2-C 4 + P2-D 3）
- [ ] Chromium 现有代码修改累计 ≤ 50 行；本 sub-plan 全部新文件在 `chrome/browser/exam/ipc/challenge/` 新目录
- [ ] 累计 ≤ 3000 行硬预算

## 文档类

- [ ] 本文件（p2-d-acceptance.md）存在
- [ ] `docs/development/ipc-challenge-schema.md` 冻结
- [ ] roadmap 里 P2 状态更新为 `⏳ 进行中 (P2-A + P2-B + P2-C + P2-D 完成)`

## 交付判定

以上全部打勾 → P2-D 完成，可进入 P2-E（最后一批：Guardian launcher 实施 + 端到端集成）。

**关键里程碑**：**P2-D 完成后 P3/P4 Guardian 端有完整的 challenge responder 规范** ——
连同 P2-A 密钥握手、P2-B wire format、P2-C 心跳，Rust/Swift Guardian **可以完全独立开发**
所有 IPC 相关代码。P2-E 只是把这些拼在一起。
```

- [ ] **Step 4: Commit acceptance + 更新 roadmap**

Run:
```bash
cd /root/byod-browser
git add docs/development/p2-d-acceptance.md
git commit -m "docs: add P2-D acceptance checklist"
```

编辑 `/root/byod-browser/docs/superpowers/plans/2026-07-11-byod-roadmap.md` 把 P2 状态改成 `⏳ 进行中 (P2-A + P2-B + P2-C + P2-D 完成)`：

```bash
cd /root/byod-browser
git add docs/superpowers/plans/2026-07-11-byod-roadmap.md
git commit -m "docs: mark P2-D complete in roadmap"
```

---

## P2-D 完成后

**已交付 3 个补丁文件**：
- `0640-add-challenge-issuer.patch` — types + issuer (30s timer + 3s timeout + JSON build)
- `0641-add-challenge-verifier-interface.patch` — 抽象接口 + NullChallengeVerifier
- `0642-add-challenge-unit-tests.patch` — 6 个 gtest 全场景

**接口冻结完成**：
- `ChallengeType` 枚举 4 值
- `ChallengeIssuer` 类
- `ChallengeVerifier` 抽象 + 全局 Set/Get
- `ipc-challenge-schema.md` 冻结（含 4 种 challenge 的 data schema + Guardian 实施建议）

**Rust (P3) / Swift (P4) Guardian 端现在可以完全独立开发**：
- Wire format v1 冻结 (P2-B)
- Challenge schema v1 冻结 (P2-D)
- 心跳协议规范清晰 (P2-C)
- 密钥握手规范清晰 (P2-A)

**下一批 P2-E**：Guardian launcher 真实实现 + 端到端集成 —— 把 KeyChannel、PeerAttestor、HeartbeatDriver、ChallengeIssuer 拼在一起，通过真实 fork 一个 Guardian binary 走全链路。预算 ~600 行（P2 里最大的一批因为是集成）。

**预计 P2-D 实际耗时**：
- 熟练 Chromium 开发者：2-3 天
- 首次改 Chromium 的开发者：4-6 天

**风险与常见坑**：
- **`base::JSONWriter::Write` API 版本**：Chromium 130+ 接受 `base::ValueView` 或 `base::Value::Dict`；老版本可能要包成 `base::Value`；按报错切
- **`base::RandBytes` 参数类型**：新版 `base::span<uint8_t>`；老版可能是 `void*, size_t`
- **`base::Base64Encode` 签名**：老 `std::string Base64Encode(base::StringPiece)`；新版可能 `Base64Encode(base::span<uint8_t>, std::string* out)`
- **随机 challenge 选择的 predictability**：`kUseRandomSelection = true` 使用 `base::RandInt`，attacker 无法预测下一个 type；deterministic rotation 也 OK 因为 Guardian 无论如何都必须响应真实数据，无法预测的收益不大
- **ExamSession 单例污染 gtest**：同 P2-C，`GTEST_SKIP` 处理
- **JSON 里 int64 变 double 精度损失**：`base::Value::Dict::Set(key, static_cast<double>(int64))` 对大数会失真；`issued_at_monotonic_ms` 用 double 存 —— 需注意 max safe integer；本项目单机时长 < 24 小时，纳秒级 double 精度足够
- **`base::Value::Dict::FindInt` 返回 `std::optional<int>`（32-bit）**：如果 schema 里 type 值 > INT_MAX 会返回 nullopt；本项目只有 4 个小 enum 值，无风险
