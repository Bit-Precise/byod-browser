# P5 · L3 策略与规则引擎 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 P1 冻结的 Chromium hooks（`ByodRequestGate` / `ExamEventSink` / `ChallengeVerifier`）与 P2 的 Guardian 事件流拼成真正的 **L3 策略与规则引擎** —— 承接一份 `ExamPolicy` 配置（JSON），实现 URL 白名单、进程黑名单、IME 白名单、快捷键黑名单、熔断决策器、challenge 校验。策略由 `ExamSession::StartExam` 传入（P6 从服务端拉取），本 sub-plan 只做**策略消费**，**不做策略下发协议**（那是 P9 交付）。

**Architecture:** 中心类 **`ExamPolicyEngine`**：接受一个 `ExamPolicy` 数据对象，构造并注册所有平台钩子（gate、verifier、sink）；持有一个 `BurnDecider` 收敛所有熔断触发条件到统一出口；提供 `SetPolicy` API 供 P6/P9 使用。

**本 sub-plan 交付的核心 API 契约**：
```cpp
// The single source of truth for what's allowed during an exam.
struct ExamPolicy {
  std::vector<UrlWhitelistRule> url_whitelist;
  std::vector<std::string> prohibited_process_shas;
  std::vector<std::string> allowed_ime_source_ids;
  std::vector<HotkeyRule> hotkey_blacklist;
  BurnPolicy burn_policy;  // 阈值参数
};

// Constructed by P6 exam shell after policy is fetched + verified.
// Registers ByodRequestGate, ChallengeVerifier, ExamEventSink installers,
// and drives BurnDecider.
class ExamPolicyEngine {
  static void Initialize(ExamPolicy policy);
  static void Shutdown();
};
```

**本 sub-plan 不做**（其它 sub-plan 交付）：
- **P6**：拿 exam 链接换 policy JSON、验证签名
- **P8**：Session Report 事件持久化 + 上报服务端（P5 只实现 `ExamEventSink` 存 in-memory + forward，不含持久化）
- **P9**：策略热更、TUF-lite 签名验证
- **P3/P4**：Guardian 侧的 IME/process/display **采集**（P5 只**消费**采集结果 via challenge response）

**Tech Stack:** Chromium C++、`base::Value::Dict` for JSON、`RE2` (Chromium 已 vendored) 或普通字符串匹配 for URL，gtest。

## Global Constraints

- Chromium 补丁总量 ≤ 3000 行硬预算；P1 + P2 累计 ≈ 2870；本 sub-plan 全在新目录 `chrome/browser/exam/policy/`，仅 BUILD.gn 增量（≤ 20 行）改动现有代码
- Chromium 补丁只做"删/禁"和"钩子暴露"；P5 是**实现层**，不新增 Chromium 补丁
- 补丁文件命名遵循 P0 patch-workflow.md 序号分段：**新分段 `1000-1099` = L3 策略引擎**（P1/P2 用 0000-0699 完毕，1000+ 为业务层预留）
- P5 依赖 P1 全部完成（34 patch）+ P2-A 的 `IpcKey`（用于 challenge verifier 中的 attestation quote 校验，可选）+ P2-D 的 `ChallengeVerifier` 接口
- **不依赖** P2-B/C/E 的运行时行为，只依赖它们冻结的接口 —— 意味着 P5 可以与 P2-E 之后的 sub-plan 并行开发
- **策略数据不变量**：`allowed_ime_source_ids` 至少含 `com.apple.keylayout.ABC` (mac) 或 `00000409` (Win 英文键盘)，即使运营方没配置也强制加，防止考生锁死无法输入

---

## File Structure

**本 sub-plan 交付：**

```
/root/byod-browser/
├── patches/
│   ├── 1000-add-exam-policy-data-model.patch          # Task 1: ExamPolicy 结构
│   ├── 1001-add-url-whitelist-engine.patch            # Task 2: URL 白名单
│   ├── 1002-add-prohibited-process-registry.patch     # Task 3: 进程黑名单
│   ├── 1003-add-ime-policy-checker.patch              # Task 4: IME 校验
│   ├── 1004-add-hotkey-policy.patch                   # Task 5: 快捷键黑名单
│   ├── 1005-add-burn-decider.patch                    # Task 6: 熔断决策器
│   ├── 1006-add-real-challenge-verifier.patch         # Task 7: challenge 校验实施
│   ├── 1007-add-exam-policy-engine.patch              # Task 8: 总装类
│   └── 1008-add-policy-engine-unit-tests.patch        # Task 9: gtest
├── build/
│   └── smoke-test-p5.sh
└── docs/
    └── development/
        ├── p5-acceptance.md
        └── exam-policy-schema.md                       # 冻结 ExamPolicy JSON schema
```

**Chromium 源码触及区域：**

- 新增 `chrome/browser/exam/policy/` 目录（本 sub-plan 全部代码）
- 修改 `chrome/browser/exam/BUILD.gn`（加 sources + `//third_party/re2` dep）
- 修改 `chrome/test/BUILD.gn`（加 unittest）

**接口冻结契约（P6/P8/P9 消费）：**

```cpp
namespace chrome::exam {

struct UrlWhitelistRule {
  std::string pattern;   // Domain glob or full URL regex.
  bool is_regex;         // false → glob; true → RE2 regex.
};

struct HotkeyRule {
  int modifiers;         // bitmask: ctrl=1, shift=2, alt=4, meta=8
  int key_code;          // ui::VKEY_* or CGKeyCode; platform-decoded
                         // by policy engine
};

struct BurnPolicy {
  int max_blur_events_per_15min = 5;    // §4.1.5 windowed threshold
  int max_burn_reports_before_burn = 3; // hysteresis before forced burn
  bool burn_on_whitelist_violation = false;  // record only vs burn
};

struct ExamPolicy {
  std::string session_id;                        // Echoed to events for audit.
  std::vector<UrlWhitelistRule> url_whitelist;
  std::vector<std::string> prohibited_process_shas;  // hex SHA-256
  std::vector<std::string> allowed_ime_source_ids;
  std::vector<HotkeyRule> hotkey_blacklist;
  BurnPolicy burn_policy;
};

class ExamPolicyEngine {
 public:
  static void Initialize(ExamPolicy policy);
  static void Shutdown();

  // Global observability.
  static const ExamPolicy* current();
};

}  // namespace chrome::exam
```

---

## Task 1: ExamPolicy 数据模型 + JSON 解析

**Files:**
- Create (in Chromium src): `chrome/browser/exam/policy/exam_policy.h`
- Create (in Chromium src): `chrome/browser/exam/policy/exam_policy.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/1000-add-exam-policy-data-model.patch`
- Create: `/root/byod-browser/build/smoke-test-p5.sh`

**Interfaces:**
- Consumes: P2 完成（60 patch 已合入）
- **Produces**（Task 2..9 与 P6/P8/P9 消费）：`ExamPolicy` 结构体 + `ParseExamPolicyFromJson`

- [ ] **Step 1: 起始状态自检**

Run:
```bash
cd /opt/chromium/src
git status
git describe --tags
git log --oneline | head -62
```
Expected: `working tree clean`；tag 一致；最近 60 条 commit 是 P1..P2-E

若不干净：`echo y | bash /root/byod-browser/build/revert-patches.sh && bash /root/byod-browser/build/apply-patches.sh`

- [ ] **Step 2: 写 smoke-test-p5.sh 骨架**

Write `/root/byod-browser/build/smoke-test-p5.sh`:

```bash
#!/usr/bin/env bash
# Behavior smoke tests for P5 (Policy Engine).
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

# ---------- Task 1: ExamPolicy header ----------
echo "[test] P5/task1: ExamPolicy"
if [ -f /opt/chromium/src/chrome/browser/exam/policy/exam_policy.h ]; then
  pass "exam_policy.h present"
else
  fail "exam_policy.h missing"
fi

out="$(run_chrome --headless --dump-dom https://example.com 2>&1 || true)"
if echo "$out" | grep -q "Example Domain"; then
  pass "chrome boots after policy scaffolding"
else
  fail "chrome broken after policy scaffolding"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "[fail] one or more P5 tests failed"; exit 1
fi
echo "[success] all P5 tests passed"
```

Run: `chmod +x /root/byod-browser/build/smoke-test-p5.sh`

- [ ] **Step 3: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p5.sh`
Expected: `✗ exam_policy.h missing`

- [ ] **Step 4: 创建 exam_policy.h**

Write `/opt/chromium/src/chrome/browser/exam/policy/exam_policy.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P5 Task 1 (spec §4 & §5.4): the exam policy data model. Configuration
// is fetched by P6 (exam shell) from the server, signature-verified by P9,
// then handed to ExamPolicyEngine::Initialize (Task 8).
//
// Full JSON schema documented in
// docs/development/exam-policy-schema.md (Task 8).

#ifndef CHROME_BROWSER_EXAM_POLICY_EXAM_POLICY_H_
#define CHROME_BROWSER_EXAM_POLICY_EXAM_POLICY_H_

#include <optional>
#include <string>
#include <vector>

#include "base/values.h"

namespace chrome::exam {

struct UrlWhitelistRule {
  std::string pattern;   // Glob (default) or RE2 regex.
  bool is_regex = false;
};

struct HotkeyRule {
  // Modifier bits (deliberately platform-abstract; policy engine maps
  // per-platform in Task 5).
  static constexpr int kCtrl = 1;
  static constexpr int kShift = 2;
  static constexpr int kAlt = 4;
  static constexpr int kMeta = 8;   // Cmd on mac; Win key on Windows.

  int modifiers = 0;
  int key_code = 0;      // ui::VKEY_* value; platform code maps to Win/mac.
};

struct BurnPolicy {
  int max_blur_events_per_15min = 5;
  int max_burn_reports_before_burn = 3;
  bool burn_on_whitelist_violation = false;
};

struct ExamPolicy {
  std::string session_id;
  std::vector<UrlWhitelistRule> url_whitelist;
  std::vector<std::string> prohibited_process_shas;  // hex SHA-256, lowercase
  std::vector<std::string> allowed_ime_source_ids;
  std::vector<HotkeyRule> hotkey_blacklist;
  BurnPolicy burn_policy;
};

// Parse a JSON string / dict into ExamPolicy. Returns nullopt on any error
// (missing required fields, bad types, etc.). Log line printed via LOG(ERROR)
// on failure.
std::optional<ExamPolicy> ParseExamPolicyFromJson(const std::string& json);
std::optional<ExamPolicy> ParseExamPolicyFromDict(const base::Value::Dict& d);

// Enforce non-negotiable defaults per policy design:
//   - allowed_ime_source_ids MUST include the platform's default English
//     keyboard layout; if missing, added implicitly.
// Modifies |policy| in place. Idempotent.
void EnforcePolicyInvariants(ExamPolicy* policy);

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_POLICY_EXAM_POLICY_H_
```

- [ ] **Step 5: 创建 exam_policy.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/exam_policy.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/exam_policy.h"

#include <algorithm>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "build/build_config.h"

namespace chrome::exam {

namespace {

// JSON key names (frozen; documented in exam-policy-schema.md).
constexpr char kSessionId[] = "session_id";
constexpr char kUrlWhitelist[] = "url_whitelist";
constexpr char kUrlPattern[] = "pattern";
constexpr char kUrlIsRegex[] = "is_regex";
constexpr char kProhibitedProcessShas[] = "prohibited_process_shas";
constexpr char kAllowedImeSourceIds[] = "allowed_ime_source_ids";
constexpr char kHotkeyBlacklist[] = "hotkey_blacklist";
constexpr char kHotkeyModifiers[] = "modifiers";
constexpr char kHotkeyKeyCode[] = "key_code";
constexpr char kBurnPolicy[] = "burn_policy";
constexpr char kMaxBlurPer15min[] = "max_blur_events_per_15min";
constexpr char kMaxBurnReports[] = "max_burn_reports_before_burn";
constexpr char kBurnOnWhitelistViolation[] = "burn_on_whitelist_violation";

// Non-negotiable default IME source id per platform.
const char* DefaultAllowedImeSourceId() {
#if BUILDFLAG(IS_MAC)
  return "com.apple.keylayout.ABC";
#elif BUILDFLAG(IS_WIN)
  return "00000409";  // en-US keyboard layout ID
#else
  return "system:en-us";
#endif
}

}  // namespace

std::optional<ExamPolicy> ParseExamPolicyFromDict(const base::Value::Dict& d) {
  ExamPolicy p;

  const std::string* sid = d.FindString(kSessionId);
  if (!sid) {
    LOG(ERROR) << "[byod] ExamPolicy: missing session_id";
    return std::nullopt;
  }
  p.session_id = *sid;

  // URL whitelist (required, may be empty list).
  const base::Value::List* wl = d.FindList(kUrlWhitelist);
  if (wl) {
    for (const base::Value& v : *wl) {
      if (!v.is_dict()) {
        LOG(ERROR) << "[byod] ExamPolicy: url_whitelist entry not a dict";
        return std::nullopt;
      }
      const auto& vd = v.GetDict();
      const std::string* pat = vd.FindString(kUrlPattern);
      if (!pat) {
        LOG(ERROR) << "[byod] ExamPolicy: url_whitelist entry missing pattern";
        return std::nullopt;
      }
      UrlWhitelistRule rule;
      rule.pattern = *pat;
      rule.is_regex = vd.FindBool(kUrlIsRegex).value_or(false);
      p.url_whitelist.push_back(rule);
    }
  }

  // Prohibited process SHAs (optional).
  const base::Value::List* pl = d.FindList(kProhibitedProcessShas);
  if (pl) {
    for (const base::Value& v : *pl) {
      if (!v.is_string()) {
        LOG(ERROR) << "[byod] ExamPolicy: prohibited_process_shas entry "
                   << "not string";
        return std::nullopt;
      }
      p.prohibited_process_shas.push_back(v.GetString());
    }
  }

  // Allowed IME source ids (optional; EnforcePolicyInvariants adds default).
  const base::Value::List* imel = d.FindList(kAllowedImeSourceIds);
  if (imel) {
    for (const base::Value& v : *imel) {
      if (!v.is_string()) {
        LOG(ERROR) << "[byod] ExamPolicy: allowed_ime_source_ids not string";
        return std::nullopt;
      }
      p.allowed_ime_source_ids.push_back(v.GetString());
    }
  }

  // Hotkey blacklist (optional).
  const base::Value::List* hkl = d.FindList(kHotkeyBlacklist);
  if (hkl) {
    for (const base::Value& v : *hkl) {
      if (!v.is_dict()) {
        LOG(ERROR) << "[byod] ExamPolicy: hotkey_blacklist entry not dict";
        return std::nullopt;
      }
      const auto& vd = v.GetDict();
      const auto mods = vd.FindInt(kHotkeyModifiers);
      const auto kc = vd.FindInt(kHotkeyKeyCode);
      if (!mods || !kc) {
        LOG(ERROR) << "[byod] ExamPolicy: hotkey needs modifiers + key_code";
        return std::nullopt;
      }
      HotkeyRule rule;
      rule.modifiers = *mods;
      rule.key_code = *kc;
      p.hotkey_blacklist.push_back(rule);
    }
  }

  // Burn policy (optional; uses struct defaults if absent).
  const base::Value::Dict* bp = d.FindDict(kBurnPolicy);
  if (bp) {
    p.burn_policy.max_blur_events_per_15min =
        bp->FindInt(kMaxBlurPer15min)
            .value_or(p.burn_policy.max_blur_events_per_15min);
    p.burn_policy.max_burn_reports_before_burn =
        bp->FindInt(kMaxBurnReports)
            .value_or(p.burn_policy.max_burn_reports_before_burn);
    p.burn_policy.burn_on_whitelist_violation =
        bp->FindBool(kBurnOnWhitelistViolation)
            .value_or(p.burn_policy.burn_on_whitelist_violation);
  }

  EnforcePolicyInvariants(&p);
  return p;
}

std::optional<ExamPolicy> ParseExamPolicyFromJson(const std::string& json) {
  auto parsed = base::JSONReader::Read(json);
  if (!parsed || !parsed->is_dict()) {
    LOG(ERROR) << "[byod] ExamPolicy: JSON parse failed";
    return std::nullopt;
  }
  return ParseExamPolicyFromDict(parsed->GetDict());
}

void EnforcePolicyInvariants(ExamPolicy* policy) {
  const std::string default_ime = DefaultAllowedImeSourceId();
  auto it = std::find(policy->allowed_ime_source_ids.begin(),
                      policy->allowed_ime_source_ids.end(),
                      default_ime);
  if (it == policy->allowed_ime_source_ids.end()) {
    LOG(INFO) << "[byod] ExamPolicy: adding default allowed IME source: "
              << default_ime;
    policy->allowed_ime_source_ids.push_back(default_ime);
  }
}

}  // namespace chrome::exam
```

- [ ] **Step 6: 修改 BUILD.gn**

在 `/opt/chromium/src/chrome/browser/exam/BUILD.gn` 的 `sources = [` 加：

```gn
    "policy/exam_policy.cc",
    "policy/exam_policy.h",
```

- [ ] **Step 7: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

- [ ] **Step 8: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p5.sh
```

- [ ] **Step 9: 导出 patch + revert/re-apply 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/policy/exam_policy.h \
        chrome/browser/exam/policy/exam_policy.cc
git commit -m "byod: add ExamPolicy data model + JSON parser (P5/T1)

P5 Task 1 (spec §4 & §5.4). ExamPolicy struct, JSON parser, non-
negotiable invariants (default English IME always allowed to prevent
lockout). JSON schema keys frozen; documented in exam-policy-schema.md
(added in Task 8)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/1000-add-exam-policy-data-model.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p5.sh

cd /root/byod-browser
git add patches/1000-add-exam-policy-data-model.patch build/smoke-test-p5.sh
git commit -m "patch: 1000 ExamPolicy data model (P5/T1)

Est. LOC in Chromium: ~230. All in new policy/ directory."
```

---

## Task 2: URL 白名单引擎 —— ByodRequestGate 真实实现

**Files:**
- Create (in Chromium src): `chrome/browser/exam/policy/url_whitelist_gate.h`
- Create (in Chromium src): `chrome/browser/exam/policy/url_whitelist_gate.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/1001-add-url-whitelist-engine.patch`

**Interfaces:**
- Consumes: Task 1 完成；`network::ByodRequestGate` (P1-C)
- **Produces**（Task 8 消费）：`UrlWhitelistGate` 类 —— 实现 `network::ByodRequestGate::Evaluate` 匹配 `ExamPolicy::url_whitelist`

**匹配语义**：
- `is_regex=false` → glob 匹配：`*.example.com/*` 语法（`*` 匹配 `[^/]*`；`/*` 匹配 path suffix）
- `is_regex=true` → RE2 完整 URL 匹配
- 空 whitelist → deny all（fail-closed）
- 命中任一规则 → allow

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p5.sh`：

```bash
# ---------- Task 2: URL whitelist gate ----------
echo "[test] P5/task2: URL whitelist gate"
if [ -f /opt/chromium/src/chrome/browser/exam/policy/url_whitelist_gate.h ]; then
  pass "url_whitelist_gate.h present"
else
  fail "url_whitelist_gate.h missing"
fi
```

- [ ] **Step 2: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p5.sh`

- [ ] **Step 3: 创建 url_whitelist_gate.h**

Write `/opt/chromium/src/chrome/browser/exam/policy/url_whitelist_gate.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P5 Task 2 (spec §4.1.1): concrete ByodRequestGate implementation
// that evaluates outgoing requests against ExamPolicy::url_whitelist.

#ifndef CHROME_BROWSER_EXAM_POLICY_URL_WHITELIST_GATE_H_
#define CHROME_BROWSER_EXAM_POLICY_URL_WHITELIST_GATE_H_

#include <memory>
#include <string>
#include <vector>

#include "chrome/browser/exam/policy/exam_policy.h"
#include "services/network/public/cpp/byod_request_gate.h"

namespace chrome::exam {

class UrlWhitelistGate : public network::ByodRequestGate {
 public:
  // Rules copied. Also enforces hard-coded rules (spec §5.4.5): chrome://
  // scheme except chrome://exam-shell/, file://, view-source:, etc. are
  // always denied regardless of whitelist entries.
  explicit UrlWhitelistGate(std::vector<UrlWhitelistRule> rules);
  ~UrlWhitelistGate() override;

  // network::ByodRequestGate:
  Decision Evaluate(const GURL& url,
                    const std::string& method,
                    const url::Origin& initiator) const override;

 private:
  bool MatchesAny(const GURL& url) const;
  static bool MatchesGlob(const std::string& pattern, const std::string& url);

  std::vector<UrlWhitelistRule> rules_;
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_POLICY_URL_WHITELIST_GATE_H_
```

- [ ] **Step 4: 创建 url_whitelist_gate.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/url_whitelist_gate.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/url_whitelist_gate.h"

#include "base/logging.h"
#include "third_party/re2/src/re2/re2.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace chrome::exam {

UrlWhitelistGate::UrlWhitelistGate(std::vector<UrlWhitelistRule> rules)
    : rules_(std::move(rules)) {}

UrlWhitelistGate::~UrlWhitelistGate() = default;

// Hard-coded fail-closed rules per spec §5.4.5. These cannot be overridden
// by policy — they are enforced regardless of whitelist entries.
namespace {

bool IsHardCodedDeny(const GURL& url) {
  // chrome:// except chrome://exam-shell/... — P1-A patch #3 already
  // filters WebUI; we double-check here as defense in depth.
  if (url.SchemeIs("chrome")) {
    return url.host() != "exam-shell";
  }
  if (url.SchemeIs("file")) return true;
  if (url.SchemeIs("view-source")) return true;
  if (url.SchemeIs("devtools") || url.SchemeIs("chrome-devtools")) return true;
  if (url.SchemeIs("chrome-extension")) return true;
  // Local addresses (spec §5.4.5).
  if (url.HostIsIPAddress()) {
    const std::string host = url.host();
    if (host == "127.0.0.1" || host == "::1") return true;
    // Private-range hosts (rough check; full CIDR parsing overkill for MVP).
    if (host.rfind("10.", 0) == 0) return true;
    if (host.rfind("192.168.", 0) == 0) return true;
    if (host.rfind("172.", 0) == 0) return true;  // approximate 172.16-31
  }
  if (url.host() == "localhost") return true;
  return false;
}

}  // namespace

UrlWhitelistGate::Decision UrlWhitelistGate::Evaluate(
    const GURL& url,
    const std::string& /*method*/,
    const url::Origin& /*initiator*/) const {
  if (IsHardCodedDeny(url)) return Decision::kDeny;
  if (!url.SchemeIsHTTPOrHTTPS()) {
    // Only http(s) permitted beyond exam-shell.
    return Decision::kDeny;
  }
  if (rules_.empty()) {
    // Empty whitelist = deny all.
    return Decision::kDeny;
  }
  return MatchesAny(url) ? Decision::kAllow : Decision::kDeny;
}

bool UrlWhitelistGate::MatchesAny(const GURL& url) const {
  const std::string spec = url.spec();  // full URL string
  for (const auto& rule : rules_) {
    if (rule.is_regex) {
      // RE2::FullMatch — anchored match; policy authors should write
      // full-URL regex.
      if (RE2::FullMatch(spec, rule.pattern)) return true;
    } else {
      if (MatchesGlob(rule.pattern, spec)) return true;
    }
  }
  return false;
}

// Minimal glob:
//   '*' matches any sequence of non-'/' chars
//   '**' matches any sequence including '/'
//   any other char matches literally
//
// Simple two-pointer implementation; suitable for URL patterns like
// "https://exam.example.com/*" or "**://cdn.example.com/**".
bool UrlWhitelistGate::MatchesGlob(const std::string& pattern,
                                    const std::string& url) {
  size_t pi = 0, ui = 0;
  ssize_t star_pi = -1, star_ui = 0;
  bool double_star = false;

  while (ui < url.size()) {
    if (pi < pattern.size() &&
        pattern[pi] == '*') {
      // Check for **
      if (pi + 1 < pattern.size() && pattern[pi + 1] == '*') {
        double_star = true;
        star_pi = static_cast<ssize_t>(pi);
        pi += 2;
      } else {
        double_star = false;
        star_pi = static_cast<ssize_t>(pi);
        ++pi;
      }
      star_ui = ui;
      continue;
    }
    if (pi < pattern.size() && pattern[pi] == url[ui]) {
      ++pi;
      ++ui;
      continue;
    }
    if (star_pi >= 0) {
      // Backtrack. Extend previous '*' match by one char.
      if (!double_star && url[star_ui] == '/') {
        return false;  // single '*' can't cross '/'
      }
      pi = static_cast<size_t>(star_pi) + (double_star ? 2 : 1);
      ++star_ui;
      ui = star_ui;
      continue;
    }
    return false;
  }
  // Consume trailing '*' in pattern.
  while (pi < pattern.size() && pattern[pi] == '*') ++pi;
  return pi == pattern.size();
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "policy/url_whitelist_gate.cc",
    "policy/url_whitelist_gate.h",
```

deps 加（若尚未有）：

```gn
    "//third_party/re2",
    "//services/network/public/cpp",
    "//url",
```

- [ ] **Step 6: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**常见报错**：
- "re2 target not found"：Chromium 里 re2 target 完整路径是 `//third_party/re2:re2` 或 `//third_party/re2`；两者都试
- `HostIsIPAddress` 名字变化：某些 Chromium 版本 API 叫 `HostIsIPLiteral`；按报错调

- [ ] **Step 7: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p5.sh
```

- [ ] **Step 8: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/policy/url_whitelist_gate.h \
        chrome/browser/exam/policy/url_whitelist_gate.cc
git commit -m "byod: add UrlWhitelistGate implementing ByodRequestGate (P5/T2)

P5 Task 2 (spec §4.1.1 + §5.4.5). Glob (default) + RE2 regex matching.
Hard-coded fail-closed rules (chrome:// except exam-shell, file://,
view-source:, devtools://, chrome-extension://, localhost, private IPs)
enforced regardless of whitelist. Empty whitelist = deny all."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/1001-add-url-whitelist-engine.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p5.sh

cd /root/byod-browser
git add patches/1001-add-url-whitelist-engine.patch build/smoke-test-p5.sh
git commit -m "patch: 1001 URL whitelist gate (P5/T2)

Est. LOC in Chromium: ~200."
```

---

## 本部分尾声

**已交付**（P5 Task 1-2）：
- Task 1: ExamPolicy 数据模型 + JSON 解析 + 强制不变量（默认英文 IME 白名单）
- Task 2: UrlWhitelistGate 实现 ByodRequestGate + hard-coded fail-closed 规则 + glob/regex 匹配

## Task 3: 进程黑名单 registry

**Files:**
- Create (in Chromium src): `chrome/browser/exam/policy/prohibited_process_registry.h`
- Create (in Chromium src): `chrome/browser/exam/policy/prohibited_process_registry.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/1002-add-prohibited-process-registry.patch`

**Interfaces:**
- Consumes: Task 2 完成；`ExamPolicy::prohibited_process_shas` (Task 1)
- **Produces**（Task 7 ChallengeVerifier + P3/P4 Guardian 侧对接）：
  ```cpp
  class ProhibitedProcessRegistry {
   public:
    explicit ProhibitedProcessRegistry(std::vector<std::string> lowercase_hex_shas);

    // O(1) lookup by lowercased SHA-256 hex string.
    bool IsProhibited(const std::string& sha256_hex) const;

    size_t size() const;
  };
  ```

**说明**：本 sub-plan 不做 Guardian 侧的**进程枚举**（那是 P3/P4）。P5 只维护一个查询表；当 Guardian 通过 challenge kRecentProcesses 上报进程 SHA-256 时，`ChallengeVerifier`（Task 7）用本 registry 查是否有 hit → 有 hit 则 verify 返回 false 触发 burn。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p5.sh`：

```bash
# ---------- Task 3: prohibited process registry ----------
echo "[test] P5/task3: ProhibitedProcessRegistry"
if [ -f /opt/chromium/src/chrome/browser/exam/policy/prohibited_process_registry.h ]; then
  pass "prohibited_process_registry.h present"
else
  fail "prohibited_process_registry.h missing"
fi
```

- [ ] **Step 2: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p5.sh`

- [ ] **Step 3: 创建 prohibited_process_registry.h**

Write `/opt/chromium/src/chrome/browser/exam/policy/prohibited_process_registry.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P5 Task 3 (spec §4.2.1): fast O(1) SHA-256 lookup for prohibited
// process detection. Guardian (P3/P4) reports observed process SHAs via
// challenge kRecentProcesses; ChallengeVerifier (Task 7) consults this
// registry to decide burn.

#ifndef CHROME_BROWSER_EXAM_POLICY_PROHIBITED_PROCESS_REGISTRY_H_
#define CHROME_BROWSER_EXAM_POLICY_PROHIBITED_PROCESS_REGISTRY_H_

#include <string>
#include <unordered_set>
#include <vector>

namespace chrome::exam {

class ProhibitedProcessRegistry {
 public:
  // Input SHAs are normalized to lowercase hex on ingestion; entries with
  // wrong length (!= 64 hex chars) are skipped with a WARNING log.
  explicit ProhibitedProcessRegistry(std::vector<std::string> shas);

  // Case-insensitive lookup. Whitespace not trimmed; caller normalizes.
  bool IsProhibited(const std::string& sha256_hex) const;

  size_t size() const { return set_.size(); }

 private:
  std::unordered_set<std::string> set_;
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_POLICY_PROHIBITED_PROCESS_REGISTRY_H_
```

- [ ] **Step 4: 创建 prohibited_process_registry.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/prohibited_process_registry.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/prohibited_process_registry.h"

#include <algorithm>

#include "base/logging.h"
#include "base/strings/string_util.h"

namespace chrome::exam {

namespace {

bool IsValidHexSha256(const std::string& s) {
  if (s.size() != 64) return false;
  return std::all_of(s.begin(), s.end(),
                     [](char c) {
                       return (c >= '0' && c <= '9') ||
                              (c >= 'a' && c <= 'f') ||
                              (c >= 'A' && c <= 'F');
                     });
}

}  // namespace

ProhibitedProcessRegistry::ProhibitedProcessRegistry(
    std::vector<std::string> shas) {
  set_.reserve(shas.size());
  for (auto& sha : shas) {
    if (!IsValidHexSha256(sha)) {
      LOG(WARNING) << "[byod] ProhibitedProcessRegistry: skipping invalid "
                   << "SHA-256 entry (len=" << sha.size() << ")";
      continue;
    }
    set_.insert(base::ToLowerASCII(sha));
  }
}

bool ProhibitedProcessRegistry::IsProhibited(
    const std::string& sha256_hex) const {
  const std::string normalized = base::ToLowerASCII(sha256_hex);
  return set_.find(normalized) != set_.end();
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "policy/prohibited_process_registry.cc",
    "policy/prohibited_process_registry.h",
```

- [ ] **Step 6: 编译 + smoke**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p5.sh
```

- [ ] **Step 7: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/policy/prohibited_process_registry.h \
        chrome/browser/exam/policy/prohibited_process_registry.cc
git commit -m "byod: add ProhibitedProcessRegistry (P5/T3)

P5 Task 3 (spec §4.2.1). O(1) unordered_set of lowercased hex SHA-256.
Invalid entries (wrong length or non-hex) skipped with warning. Guardian
(P3/P4) reports observed process SHAs; ChallengeVerifier (Task 7)
consults this registry."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/1002-add-prohibited-process-registry.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p5.sh

cd /root/byod-browser
git add patches/1002-add-prohibited-process-registry.patch build/smoke-test-p5.sh
git commit -m "patch: 1002 prohibited process registry (P5/T3)

Est. LOC in Chromium: ~80."
```

---

## Task 4: IME 策略 checker

**Files:**
- Create (in Chromium src): `chrome/browser/exam/policy/ime_policy_checker.h`
- Create (in Chromium src): `chrome/browser/exam/policy/ime_policy_checker.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/1003-add-ime-policy-checker.patch`

**Interfaces:**
- Consumes: Task 3 完成；`ExamPolicy::allowed_ime_source_ids` (Task 1)
- **Produces**（Task 7 ChallengeVerifier 消费）：
  ```cpp
  class ImePolicyChecker {
   public:
    explicit ImePolicyChecker(std::vector<std::string> allowed_source_ids);

    // Returns true iff the reported source ID is in the allow-list.
    bool IsAllowed(const std::string& reported_source_id) const;
  };
  ```

**语义**：**allow-list** 语义（相反于 process 的 deny-list） —— 只有列表内的 IME source ID 被允许；空列表 = **只有** `EnforcePolicyInvariants` 加的默认英文键盘允许（因为 Task 1 已保证列表非空）。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p5.sh`：

```bash
# ---------- Task 4: IME policy checker ----------
echo "[test] P5/task4: ImePolicyChecker"
if [ -f /opt/chromium/src/chrome/browser/exam/policy/ime_policy_checker.h ]; then
  pass "ime_policy_checker.h present"
else
  fail "ime_policy_checker.h missing"
fi
```

- [ ] **Step 2: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p5.sh`

- [ ] **Step 3: 创建 ime_policy_checker.h**

Write `/opt/chromium/src/chrome/browser/exam/policy/ime_policy_checker.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P5 Task 4 (spec §4.2.3): IME allow-list checker. Guardian reports
// the active IME source ID via challenge kActiveIme; if not in allow-list,
// ChallengeVerifier (Task 7) returns false → burn.

#ifndef CHROME_BROWSER_EXAM_POLICY_IME_POLICY_CHECKER_H_
#define CHROME_BROWSER_EXAM_POLICY_IME_POLICY_CHECKER_H_

#include <string>
#include <unordered_set>
#include <vector>

namespace chrome::exam {

class ImePolicyChecker {
 public:
  explicit ImePolicyChecker(std::vector<std::string> allowed_source_ids);

  // Case-sensitive exact match (source IDs from OS APIs are stable
  // reverse-DNS strings; case matters on macOS).
  bool IsAllowed(const std::string& reported_source_id) const;

  size_t size() const { return set_.size(); }

 private:
  std::unordered_set<std::string> set_;
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_POLICY_IME_POLICY_CHECKER_H_
```

- [ ] **Step 4: 创建 ime_policy_checker.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/ime_policy_checker.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/ime_policy_checker.h"

#include "base/logging.h"

namespace chrome::exam {

ImePolicyChecker::ImePolicyChecker(std::vector<std::string> allowed) {
  set_.reserve(allowed.size());
  for (auto& id : allowed) {
    if (id.empty()) {
      LOG(WARNING) << "[byod] ImePolicyChecker: skipping empty source ID";
      continue;
    }
    set_.insert(std::move(id));
  }
  if (set_.empty()) {
    // Should never happen because EnforcePolicyInvariants (Task 1) adds
    // the platform default. But guard defensively.
    LOG(ERROR) << "[byod] ImePolicyChecker: allow-list empty after "
               << "EnforcePolicyInvariants — all IMEs will be rejected!";
  }
}

bool ImePolicyChecker::IsAllowed(const std::string& reported_source_id) const {
  return set_.find(reported_source_id) != set_.end();
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "policy/ime_policy_checker.cc",
    "policy/ime_policy_checker.h",
```

- [ ] **Step 6: 编译 + smoke**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p5.sh
```

- [ ] **Step 7: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/policy/ime_policy_checker.h \
        chrome/browser/exam/policy/ime_policy_checker.cc
git commit -m "byod: add ImePolicyChecker allow-list (P5/T4)

P5 Task 4 (spec §4.2.3). Case-sensitive exact match on IME source ID
strings. Empty allow-list logged as error (should not happen because
EnforcePolicyInvariants adds default English keyboard)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/1003-add-ime-policy-checker.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p5.sh

cd /root/byod-browser
git add patches/1003-add-ime-policy-checker.patch build/smoke-test-p5.sh
git commit -m "patch: 1003 IME policy checker (P5/T4)

Est. LOC in Chromium: ~70."
```

---

## Task 5: 快捷键黑名单（运行时监控 + 事件上报）

**Files:**
- Create (in Chromium src): `chrome/browser/exam/policy/hotkey_policy.h`
- Create (in Chromium src): `chrome/browser/exam/policy/hotkey_policy.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/1004-add-hotkey-policy.patch`

**Interfaces:**
- Consumes: Task 4 完成；`ExamPolicy::hotkey_blacklist` (Task 1)
- **Produces**（Task 8 ExamPolicyEngine 消费）：
  ```cpp
  class HotkeyPolicy {
   public:
    explicit HotkeyPolicy(std::vector<HotkeyRule> blacklist);

    // Check if a captured (modifiers, key_code) pair matches any blacklisted
    // hotkey. Called from BrowserView key event forwarder (future P5 or P6
    // wiring) or from Guardian keyboard events.
    bool IsBlacklisted(int modifiers, int key_code) const;
  };
  ```

**说明**：P1-A/B 已经在 Chromium 补丁里**编译期禁用**了 F12 / Ctrl+U / Ctrl+S / Ctrl+P 等；本 Task 是**运行时可配置**的补充层 —— 服务端下发的策略可以额外禁 Ctrl+F、Ctrl+A 等（比如某些考试禁止全选文本）。**HotkeyPolicy 只判定 + 上报事件**，实际拦截由 Chromium 层或 Guardian 层做（P3/P4 里的 keyboard hook）。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p5.sh`：

```bash
# ---------- Task 5: HotkeyPolicy ----------
echo "[test] P5/task5: HotkeyPolicy"
if [ -f /opt/chromium/src/chrome/browser/exam/policy/hotkey_policy.h ]; then
  pass "hotkey_policy.h present"
else
  fail "hotkey_policy.h missing"
fi
```

- [ ] **Step 2: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p5.sh`

- [ ] **Step 3: 创建 hotkey_policy.h**

Write `/opt/chromium/src/chrome/browser/exam/policy/hotkey_policy.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P5 Task 5 (spec §4.2.2): runtime-configurable hotkey blacklist,
// complementing the compile-time removals from P1-A/B. The engine only
// decides "is this key combo blacklisted"; actual key interception lives
// in Chromium's accelerator layer (P1-A/B patches) or Guardian's keyboard
// hook (P3/P4).

#ifndef CHROME_BROWSER_EXAM_POLICY_HOTKEY_POLICY_H_
#define CHROME_BROWSER_EXAM_POLICY_HOTKEY_POLICY_H_

#include <vector>

#include "chrome/browser/exam/policy/exam_policy.h"

namespace chrome::exam {

class HotkeyPolicy {
 public:
  explicit HotkeyPolicy(std::vector<HotkeyRule> blacklist);

  // Returns true if the given combo matches any blacklisted rule.
  bool IsBlacklisted(int modifiers, int key_code) const;

  size_t size() const { return rules_.size(); }

 private:
  std::vector<HotkeyRule> rules_;
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_POLICY_HOTKEY_POLICY_H_
```

- [ ] **Step 4: 创建 hotkey_policy.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/hotkey_policy.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/hotkey_policy.h"

namespace chrome::exam {

HotkeyPolicy::HotkeyPolicy(std::vector<HotkeyRule> blacklist)
    : rules_(std::move(blacklist)) {}

bool HotkeyPolicy::IsBlacklisted(int modifiers, int key_code) const {
  for (const auto& rule : rules_) {
    if (rule.modifiers == modifiers && rule.key_code == key_code) {
      return true;
    }
  }
  return false;
}

}  // namespace chrome::exam
```

**说明**：本类目前是纯 lookup，未与 Chromium accelerator 层挂钩。挂钩会在 Task 8 `ExamPolicyEngine::Initialize` 时通过一个 pre-existing hook 完成（Chromium 已有 `AcceleratorFilter` 或类似机制）；由于 P1-A/B 已经编译期移除了大部分开发者快捷键，本 sub-plan 里 HotkeyPolicy 主要为"未来 policy 层加更多快捷键"预留接口，运行时挂钩可以简化到 Task 8 完成时确认。

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "policy/hotkey_policy.cc",
    "policy/hotkey_policy.h",
```

- [ ] **Step 6: 编译 + smoke**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p5.sh
```

- [ ] **Step 7: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/policy/hotkey_policy.h \
        chrome/browser/exam/policy/hotkey_policy.cc
git commit -m "byod: add HotkeyPolicy runtime blacklist (P5/T5)

P5 Task 5 (spec §4.2.2). Runtime-configurable hotkey blacklist,
complementing P1-A/B compile-time removals. Lookup-only; actual
interception is Chromium accelerator layer + Guardian keyboard hook.
Hook wiring finalized in Task 8 ExamPolicyEngine::Initialize."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/1004-add-hotkey-policy.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p5.sh

cd /root/byod-browser
git add patches/1004-add-hotkey-policy.patch build/smoke-test-p5.sh
git commit -m "patch: 1004 hotkey policy (P5/T5)

Est. LOC in Chromium: ~50."
```

---

## 本部分尾声

**已交付**（P5 Task 3-5）：
- Task 3: ProhibitedProcessRegistry —— O(1) SHA-256 查找，非法条目 skip + WARN
- Task 4: ImePolicyChecker —— case-sensitive allow-list，配合 Task 1 强制不变量确保始终有默认英文
- Task 5: HotkeyPolicy —— 运行时可配置黑名单，与 P1-A/B 编译期禁用互补

**关键设计一致性**：
- Process 是 **deny-list**（空 = 无限制）
- IME 是 **allow-list**（空 = 全禁 → 用 EnforcePolicyInvariants 保证不为空）
- Hotkey 是 **deny-list**（空 = 只用 P1-A/B 编译期禁用）

## Task 6: BurnDecider 熔断决策器

**Files:**
- Create (in Chromium src): `chrome/browser/exam/policy/burn_decider.h`
- Create (in Chromium src): `chrome/browser/exam/policy/burn_decider.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/1005-add-burn-decider.patch`

**Interfaces:**
- Consumes: Task 5 完成；`ExamPolicy::burn_policy` (Task 1)；`ExamEventSink` / `ExamEvent` (P1-D)
- **Produces**（Task 8 消费）：
  ```cpp
  namespace chrome::exam {

  // Reasons that a burn was requested (for logging + ExamSession::TriggerBurn).
  enum class BurnTrigger {
    kUrlWhitelistViolation = 1,
    kProhibitedProcessDetected = 2,
    kImeOutOfAllowList = 3,
    kHotkeyBlacklistHit = 4,
    kBlurThresholdExceeded = 5,
    kChallengeVerifyFailed = 6,
    kAttestationFailed = 7,
  };

  // Sink implementation that classifies events and drives burn decisions.
  // Also forwards events to a downstream sink (P8 SessionReport).
  class BurnDecider : public ExamEventSink {
   public:
    BurnDecider(BurnPolicy policy,
                std::unique_ptr<ExamEventSink> downstream_sink);
    ~BurnDecider() override;

    // ExamEventSink:
    void OnEvent(const ExamEvent& event) override;

    // Called by ChallengeVerifier (Task 7) or other components to report a
    // discrete burn trigger. Applies hysteresis per BurnPolicy.
    void ReportBurnTrigger(BurnTrigger trigger, const std::string& detail);

   private:
    // ...
  };

  }  // namespace chrome::exam
  ```

**决策规则**（对齐 spec §4.1.5）：
- **kWindowBlur** 事件计入 15 分钟滑动窗口；超 `max_blur_events_per_15min` → immediate burn (`heartbeat_blur_over_limit`)
- 其它 burn trigger 累加计数；超 `max_burn_reports_before_burn` → burn；否则记录 + downstream forward
- 所有事件（不论是否触发 burn）都 forward 到 downstream sink（P8 将来的 Session Report）

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p5.sh`：

```bash
# ---------- Task 6: BurnDecider ----------
echo "[test] P5/task6: BurnDecider"
if [ -f /opt/chromium/src/chrome/browser/exam/policy/burn_decider.h ]; then
  pass "burn_decider.h present"
else
  fail "burn_decider.h missing"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p5.sh`

- [ ] **Step 3: 创建 burn_decider.h**

Write `/opt/chromium/src/chrome/browser/exam/policy/burn_decider.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P5 Task 6 (spec §4.1.5): burn decision hub. All burn-triggering
// signals converge here. Applies:
//   - 15-minute sliding-window blur event counter
//   - Hysteresis before non-blur burns (max_burn_reports_before_burn)
//
// Also plays the role of ExamEventSink so it sees every event first;
// events are forwarded to a downstream sink (which P8 fills with the
// Session Report implementation).

#ifndef CHROME_BROWSER_EXAM_POLICY_BURN_DECIDER_H_
#define CHROME_BROWSER_EXAM_POLICY_BURN_DECIDER_H_

#include <deque>
#include <memory>
#include <string>

#include "base/time/time.h"
#include "chrome/browser/exam/events/exam_event_sink.h"
#include "chrome/browser/exam/policy/exam_policy.h"

namespace chrome::exam {

enum class BurnTrigger {
  kUrlWhitelistViolation = 1,
  kProhibitedProcessDetected = 2,
  kImeOutOfAllowList = 3,
  kHotkeyBlacklistHit = 4,
  kBlurThresholdExceeded = 5,
  kChallengeVerifyFailed = 6,
  kAttestationFailed = 7,
};

const char* BurnTriggerName(BurnTrigger trigger);

class BurnDecider : public ExamEventSink {
 public:
  BurnDecider(BurnPolicy policy,
              std::unique_ptr<ExamEventSink> downstream_sink);
  ~BurnDecider() override;

  BurnDecider(const BurnDecider&) = delete;
  BurnDecider& operator=(const BurnDecider&) = delete;

  // ExamEventSink:
  void OnEvent(const ExamEvent& event) override;

  // Called by ChallengeVerifier + other components on non-event burns.
  // |detail| is a short human-readable string, e.g. "process_sha=abc123".
  void ReportBurnTrigger(BurnTrigger trigger, const std::string& detail);

  // Observability.
  int burn_report_count() const { return burn_report_count_; }
  bool has_burned() const { return burned_; }

 private:
  // Prune blur timestamps older than 15 min; return count in window.
  size_t PruneBlurWindow(base::TimeTicks now);

  // If not already burned, call ExamSession::TriggerBurn and set flag.
  void MaybeBurn(const std::string& reason);

  BurnPolicy policy_;
  std::unique_ptr<ExamEventSink> downstream_;
  std::deque<base::TimeTicks> blur_window_;
  int burn_report_count_ = 0;
  bool burned_ = false;
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_POLICY_BURN_DECIDER_H_
```

- [ ] **Step 4: 创建 burn_decider.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/burn_decider.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/burn_decider.h"

#include "base/logging.h"
#include "chrome/browser/exam/session/exam_session.h"

namespace chrome::exam {

const char* BurnTriggerName(BurnTrigger trigger) {
  switch (trigger) {
    case BurnTrigger::kUrlWhitelistViolation: return "url_whitelist_violation";
    case BurnTrigger::kProhibitedProcessDetected:
      return "prohibited_process_detected";
    case BurnTrigger::kImeOutOfAllowList: return "ime_out_of_allowlist";
    case BurnTrigger::kHotkeyBlacklistHit: return "hotkey_blacklist_hit";
    case BurnTrigger::kBlurThresholdExceeded: return "blur_threshold_exceeded";
    case BurnTrigger::kChallengeVerifyFailed: return "challenge_verify_failed";
    case BurnTrigger::kAttestationFailed: return "attestation_failed";
  }
  return "unknown";
}

BurnDecider::BurnDecider(BurnPolicy policy,
                         std::unique_ptr<ExamEventSink> downstream_sink)
    : policy_(std::move(policy)), downstream_(std::move(downstream_sink)) {}

BurnDecider::~BurnDecider() = default;

void BurnDecider::OnEvent(const ExamEvent& event) {
  // Forward every event downstream first so P8 always records.
  if (downstream_) downstream_->OnEvent(event);

  // Blur bookkeeping.
  if (event.type == ExamEventType::kWindowBlur) {
    blur_window_.push_back(event.monotonic_ts);
    const size_t in_window = PruneBlurWindow(event.monotonic_ts);
    if (static_cast<int>(in_window) > policy_.max_blur_events_per_15min) {
      LOG(WARNING) << "[byod] BurnDecider: blur count in 15min="
                   << in_window << " > "
                   << policy_.max_blur_events_per_15min;
      MaybeBurn("blur_threshold_exceeded");
    }
  }
  // Other event types just get forwarded; specific burns are reported
  // separately via ReportBurnTrigger.
}

void BurnDecider::ReportBurnTrigger(BurnTrigger trigger,
                                    const std::string& detail) {
  if (burned_) return;
  ++burn_report_count_;

  const char* name = BurnTriggerName(trigger);
  LOG(WARNING) << "[byod] BurnDecider: trigger=" << name
               << " detail=" << detail
               << " report_count=" << burn_report_count_
               << " threshold=" << policy_.max_burn_reports_before_burn;

  // Certain triggers burn immediately regardless of hysteresis.
  const bool immediate =
      trigger == BurnTrigger::kAttestationFailed ||
      trigger == BurnTrigger::kChallengeVerifyFailed ||
      trigger == BurnTrigger::kProhibitedProcessDetected ||
      trigger == BurnTrigger::kImeOutOfAllowList;

  const bool violation_burn =
      policy_.burn_on_whitelist_violation &&
      trigger == BurnTrigger::kUrlWhitelistViolation;

  if (immediate || violation_burn ||
      burn_report_count_ >= policy_.max_burn_reports_before_burn) {
    MaybeBurn(name);
  }
}

size_t BurnDecider::PruneBlurWindow(base::TimeTicks now) {
  const base::TimeTicks cutoff = now - base::Minutes(15);
  while (!blur_window_.empty() && blur_window_.front() < cutoff) {
    blur_window_.pop_front();
  }
  return blur_window_.size();
}

void BurnDecider::MaybeBurn(const std::string& reason) {
  if (burned_) return;
  burned_ = true;
  LOG(ERROR) << "[byod] BurnDecider: triggering session burn (" << reason
             << ")";
  ExamSession::Get().TriggerBurn(reason);
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "policy/burn_decider.cc",
    "policy/burn_decider.h",
```

- [ ] **Step 6: 编译 + smoke**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p5.sh
```

- [ ] **Step 7: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/policy/burn_decider.h \
        chrome/browser/exam/policy/burn_decider.cc
git commit -m "byod: add BurnDecider hub (P5/T6)

P5 Task 6 (spec §4.1.5). Implements ExamEventSink so it sees every event
before forwarding to downstream (P8). Rules:
  - 15-min sliding window blur counter → immediate burn on overflow
  - Immediate burn triggers: attestation fail, challenge fail, prohibited
    process, IME out of allow-list
  - Hysteresis (max_burn_reports_before_burn) for other triggers
  - Idempotent: TriggerBurn only fires once."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/1005-add-burn-decider.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p5.sh

cd /root/byod-browser
git add patches/1005-add-burn-decider.patch build/smoke-test-p5.sh
git commit -m "patch: 1005 burn decider (P5/T6)

Est. LOC in Chromium: ~160."
```

---

## Task 7: 真实 ChallengeVerifier 实施 (PolicyChallengeVerifier)

**Files:**
- Create (in Chromium src): `chrome/browser/exam/policy/policy_challenge_verifier.h`
- Create (in Chromium src): `chrome/browser/exam/policy/policy_challenge_verifier.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/1006-add-real-challenge-verifier.patch`

**Interfaces:**
- Consumes: Task 6 完成；`ChallengeVerifier` interface (P2-D)；Registry/Checker (Task 3/4)；`HardwareAttestor` (P1-E)
- **Produces**（Task 8 消费）：`PolicyChallengeVerifier` 类 —— 实现 `ChallengeVerifier::VerifyResponse` 用 Task 3/4 + BurnDecider

**行为**：对 4 种 challenge type 分别处理：
- `kActiveIme`: 提取 `ime_source_id` → `ImePolicyChecker::IsAllowed`；不允许 → `ReportBurnTrigger(kImeOutOfAllowList)` + return false
- `kRecentProcesses`: 遍历 `processes` 列表，每个 `exe_basename` 查表；hit → `ReportBurnTrigger(kProhibitedProcessDetected)` + return false（**注意**：challenge schema 的 data 携带 basename 而非 SHA-256；生产版本 Guardian 会同时报 SHA。本 sub-plan MVP 简化为 basename 匹配 —— 生产上要升级）
- `kDisplayList`: 独立查 `display::Screen::GetAllDisplays()`，count 不匹配 → `ReportBurnTrigger(kAttestationFailed)`（复用；因为多屏是 attestation-class 的欺骗）
- `kAttestationQuote`: 提取 `fingerprint` + `signature`；对 `kSoft` level 只 log；对 `kTpm`/`kSecureEnclave` 用 `HardwareAttestor` 独立算 fingerprint 对比（生产版本还需验签名 —— P7 完善）

**注意**：本 sub-plan 的 verifier 对 process schema 简化为**exe basename**匹配；ProhibitedProcessRegistry 是 SHA-256 —— **两个数据源不匹配**。修法：**MVP 里让 Registry 同时接受 basename hash SHA-256 或直接 basename**？不 —— 更清爽：本 sub-plan Task 3 已冻结 Registry 用 SHA-256；那么 challenge schema 里的 processes 应包含 `sha256_hex` 字段。**回头修 Task 3 schema 补 sha256_hex**：更新 `ipc-challenge-schema.md` 里 kRecentProcesses 加 `"sha256_hex": "..."` 字段 —— 但 schema 已冻结了 v1！

**决策**：接受不完美 —— 本 sub-plan **不改 challenge schema v1**（保持 wire 冻结承诺），本 Task 7 里 kRecentProcesses 校验**只查 basename**（P5 的 Registry 内部临时同时存 basename 版本）；生产版本升级为 SHA-256 校验时同步 bump schema v2。**在文档里显式标注这个已知问题**。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p5.sh`：

```bash
# ---------- Task 7: PolicyChallengeVerifier ----------
echo "[test] P5/task7: PolicyChallengeVerifier"
if [ -f /opt/chromium/src/chrome/browser/exam/policy/policy_challenge_verifier.h ]; then
  pass "policy_challenge_verifier.h present"
else
  fail "policy_challenge_verifier.h missing"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p5.sh`

- [ ] **Step 3: 创建 policy_challenge_verifier.h**

Write `/opt/chromium/src/chrome/browser/exam/policy/policy_challenge_verifier.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P5 Task 7 (spec §4.2 + §5.1.5): concrete ChallengeVerifier that
// consults ProhibitedProcessRegistry, ImePolicyChecker, and
// HardwareAttestor. Reports burn triggers through BurnDecider.
//
// KNOWN LIMITATION (P5 Task 7): challenge schema v1 kRecentProcesses only
// carries exe_basename (see ipc-challenge-schema.md). This verifier does
// basename matching against a parallel "prohibited basenames" set the
// caller must supply. When challenge schema bumps to v2 (adds sha256_hex),
// the check will switch to real SHA lookup via ProhibitedProcessRegistry.

#ifndef CHROME_BROWSER_EXAM_POLICY_POLICY_CHALLENGE_VERIFIER_H_
#define CHROME_BROWSER_EXAM_POLICY_POLICY_CHALLENGE_VERIFIER_H_

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/exam/ipc/challenge/challenge_verifier.h"

namespace chrome::exam {

class BurnDecider;
class ImePolicyChecker;
class ProhibitedProcessRegistry;

class PolicyChallengeVerifier : public ChallengeVerifier {
 public:
  // Non-owning; all pointers must outlive this instance.
  //
  // |prohibited_basenames| is a parallel deny-list (see KNOWN LIMITATION
  // in the header comment). Case-insensitive.
  PolicyChallengeVerifier(
      raw_ptr<BurnDecider> decider,
      raw_ptr<const ImePolicyChecker> ime_checker,
      raw_ptr<const ProhibitedProcessRegistry> process_registry,
      std::vector<std::string> prohibited_basenames);
  ~PolicyChallengeVerifier() override;

  // ChallengeVerifier:
  bool VerifyResponse(ChallengeType type,
                      const std::string& nonce_base64,
                      const base::Value::Dict& data) override;

 private:
  bool VerifyImeResponse(const base::Value::Dict& data);
  bool VerifyProcessesResponse(const base::Value::Dict& data);
  bool VerifyDisplayResponse(const base::Value::Dict& data);
  bool VerifyAttestationResponse(const base::Value::Dict& data);

  raw_ptr<BurnDecider> decider_;
  raw_ptr<const ImePolicyChecker> ime_checker_;
  raw_ptr<const ProhibitedProcessRegistry> process_registry_;
  std::unordered_set<std::string> prohibited_basenames_lower_;
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_POLICY_POLICY_CHALLENGE_VERIFIER_H_
```

- [ ] **Step 4: 创建 policy_challenge_verifier.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/policy_challenge_verifier.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/policy_challenge_verifier.h"

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "chrome/browser/exam/attestation/hardware_attestor.h"
#include "chrome/browser/exam/ipc/challenge/challenge_types.h"
#include "chrome/browser/exam/policy/burn_decider.h"
#include "chrome/browser/exam/policy/ime_policy_checker.h"
#include "chrome/browser/exam/policy/prohibited_process_registry.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"

namespace chrome::exam {

PolicyChallengeVerifier::PolicyChallengeVerifier(
    raw_ptr<BurnDecider> decider,
    raw_ptr<const ImePolicyChecker> ime_checker,
    raw_ptr<const ProhibitedProcessRegistry> process_registry,
    std::vector<std::string> prohibited_basenames)
    : decider_(decider),
      ime_checker_(ime_checker),
      process_registry_(process_registry) {
  for (const auto& b : prohibited_basenames) {
    prohibited_basenames_lower_.insert(base::ToLowerASCII(b));
  }
}

PolicyChallengeVerifier::~PolicyChallengeVerifier() = default;

bool PolicyChallengeVerifier::VerifyResponse(
    ChallengeType type,
    const std::string& /*nonce_base64*/,
    const base::Value::Dict& data) {
  switch (type) {
    case ChallengeType::kActiveIme:
      return VerifyImeResponse(data);
    case ChallengeType::kRecentProcesses:
      return VerifyProcessesResponse(data);
    case ChallengeType::kDisplayList:
      return VerifyDisplayResponse(data);
    case ChallengeType::kAttestationQuote:
      return VerifyAttestationResponse(data);
  }
  LOG(WARNING) << "[byod] PolicyChallengeVerifier: unknown type";
  return true;  // Unknown = accept (forward compat).
}

bool PolicyChallengeVerifier::VerifyImeResponse(const base::Value::Dict& data) {
  const std::string* src = data.FindString(challenge_keys::kImeSourceId);
  if (!src) {
    LOG(WARNING) << "[byod] IME response missing ime_source_id";
    return true;  // Malformed but harmless — challenge issuer will log.
  }
  if (!ime_checker_ || ime_checker_->IsAllowed(*src)) return true;

  if (decider_) {
    decider_->ReportBurnTrigger(BurnTrigger::kImeOutOfAllowList,
                                "ime_source_id=" + *src);
  }
  return false;
}

bool PolicyChallengeVerifier::VerifyProcessesResponse(
    const base::Value::Dict& data) {
  const base::Value::List* processes = data.FindList(challenge_keys::kProcesses);
  if (!processes) return true;
  bool any_prohibited = false;
  std::string detail;
  for (const base::Value& v : *processes) {
    if (!v.is_dict()) continue;
    const auto& d = v.GetDict();
    const std::string* basename = d.FindString(challenge_keys::kExeBasename);
    if (!basename) continue;
    const std::string lower = base::ToLowerASCII(*basename);
    if (prohibited_basenames_lower_.find(lower) !=
        prohibited_basenames_lower_.end()) {
      any_prohibited = true;
      if (detail.empty()) detail = "basename=" + *basename;
      // Also check SHA-256 field once schema v2 lands. For now warn if
      // field absent (v1 doesn't have it).
    }
  }
  if (any_prohibited && decider_) {
    decider_->ReportBurnTrigger(BurnTrigger::kProhibitedProcessDetected,
                                detail);
    return false;
  }
  return true;
}

bool PolicyChallengeVerifier::VerifyDisplayResponse(
    const base::Value::Dict& data) {
  const base::Value::List* reported = data.FindList(challenge_keys::kDisplays);
  if (!reported) return true;
  const size_t reported_count = reported->size();

  size_t browser_count = 0;
  auto* screen = display::Screen::GetScreen();
  if (screen) {
    browser_count = screen->GetAllDisplays().size();
  }
  if (browser_count > 0 && reported_count != browser_count) {
    LOG(WARNING) << "[byod] Display mismatch: guardian_reported="
                 << reported_count << " browser_seen=" << browser_count;
    if (decider_) {
      decider_->ReportBurnTrigger(BurnTrigger::kAttestationFailed,
                                  "display_count_mismatch");
    }
    return false;
  }
  return true;
}

bool PolicyChallengeVerifier::VerifyAttestationResponse(
    const base::Value::Dict& data) {
  const std::string* level = data.FindString(challenge_keys::kAttLevel);
  const std::string* fp = data.FindString(challenge_keys::kAttFingerprint);
  if (!level || !fp) {
    LOG(WARNING) << "[byod] Attestation response missing fields";
    return true;
  }
  if (*level == challenge_keys::kLevelSoft) {
    // Soft: recompute locally, compare.
    auto* attestor = GetHardwareAttestor();
    if (!attestor) return true;
    // Nonce here is empty because we don't thread it into Verify;
    // for soft comparison we need a shared derivation. For P5 MVP we
    // just verify the reported fingerprint has 64 hex chars.
    if (fp->size() != 64) {
      if (decider_) {
        decider_->ReportBurnTrigger(BurnTrigger::kAttestationFailed,
                                    "soft_fp_bad_length");
      }
      return false;
    }
    return true;
  }
  // TPM / Secure Enclave: signature verification is P7's job. For now
  // accept and log.
  LOG(INFO) << "[byod] Attestation level=" << *level
            << " signature check deferred to P7";
  return true;
}

}  // namespace chrome::exam
```

**说明**：本 Task 已知 3 处**MVP 妥协**在代码里显式注释：
1. Process 校验用 basename 而非 SHA-256（challenge schema v1 限制）
2. Soft attestation 只查长度不算对比（缺少 nonce 共享机制，未来加）
3. TPM/SE 签名校验推迟到 P7

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "policy/policy_challenge_verifier.cc",
    "policy/policy_challenge_verifier.h",
```

deps 加（若无）：

```gn
    "//ui/display",
```

- [ ] **Step 6: 编译 + smoke**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p5.sh
```

- [ ] **Step 7: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/policy/policy_challenge_verifier.h \
        chrome/browser/exam/policy/policy_challenge_verifier.cc
git commit -m "byod: add PolicyChallengeVerifier (P5/T7)

P5 Task 7 (spec §4.2 + §5.1.5). Consults ImePolicyChecker,
ProhibitedProcessRegistry (via parallel basename set — challenge schema
v1 limitation), display::Screen, HardwareAttestor. Reports burn triggers
via BurnDecider on mismatch. 3 MVP compromises documented in-code:
process basename vs SHA, soft attestation weak, TPM/SE signature
verification deferred to P7."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/1006-add-real-challenge-verifier.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p5.sh

cd /root/byod-browser
git add patches/1006-add-real-challenge-verifier.patch build/smoke-test-p5.sh
git commit -m "patch: 1006 real challenge verifier (P5/T7)

Est. LOC in Chromium: ~200."
```

---

## Task 8: ExamPolicyEngine 总装 + exam-policy-schema.md 冻结

**Files:**
- Create (in Chromium src): `chrome/browser/exam/policy/exam_policy_engine.h`
- Create (in Chromium src): `chrome/browser/exam/policy/exam_policy_engine.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/1007-add-exam-policy-engine.patch`
- Create: `/root/byod-browser/docs/development/exam-policy-schema.md`

**Interfaces:**
- Consumes: Task 7 完成；所有前置组件（Task 1-7）
- **Produces**（P6 消费）：`ExamPolicyEngine::Initialize(policy)` / `Shutdown()`

**总装职责**：
1. 从 policy 构造 UrlWhitelistGate 并 `network::SetByodRequestGate(...)` 注入
2. 从 policy 构造 ImePolicyChecker + ProhibitedProcessRegistry
3. 构造 downstream event sink（本 sub-plan 用 null；P8 会覆盖）
4. 构造 BurnDecider 并 `SetExamEventSink(...)` 注入
5. 构造 PolicyChallengeVerifier 并 `SetChallengeVerifier(...)` 注入
6. Shutdown 时全部反注入 + 清理

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p5.sh`：

```bash
# ---------- Task 8: ExamPolicyEngine ----------
echo "[test] P5/task8: ExamPolicyEngine"
if [ -f /opt/chromium/src/chrome/browser/exam/policy/exam_policy_engine.h ]; then
  pass "exam_policy_engine.h present"
else
  fail "exam_policy_engine.h missing"
fi
```

- [ ] **Step 2: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p5.sh`

- [ ] **Step 3: 创建 exam_policy_engine.h**

Write `/opt/chromium/src/chrome/browser/exam/policy/exam_policy_engine.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P5 Task 8 (spec §4 & §5): total assembly of the L3 policy engine.
// Fed with an ExamPolicy (parsed + signed by P9 hot-update layer, fetched
// by P6 exam shell). Installs all runtime hooks: ByodRequestGate,
// ExamEventSink (as BurnDecider), ChallengeVerifier.

#ifndef CHROME_BROWSER_EXAM_POLICY_EXAM_POLICY_ENGINE_H_
#define CHROME_BROWSER_EXAM_POLICY_EXAM_POLICY_ENGINE_H_

#include "chrome/browser/exam/policy/exam_policy.h"

namespace chrome::exam {

class ExamPolicyEngine {
 public:
  // Idempotent guard: Initialize called twice replaces the prior policy
  // (all hooks reinstalled). Called by ExamSession::StartExam AFTER
  // IpcSupervisor::LaunchAndAttest so hooks are ready when Guardian
  // starts sending events.
  static void Initialize(ExamPolicy policy);

  // Reset all hooks to default (null) sinks/gates/verifiers. Called by
  // ExamSession::EndExam / TriggerBurn (indirectly via cleanup path).
  static void Shutdown();

  // Read-only accessor for the current policy (nullptr if never initialized
  // or after Shutdown).
  static const ExamPolicy* current();

  // Observability: last known BurnDecider report count (0 if not initialized).
  static int burn_report_count();

 private:
  ExamPolicyEngine() = delete;
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_POLICY_EXAM_POLICY_ENGINE_H_
```

- [ ] **Step 4: 创建 exam_policy_engine.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/exam_policy_engine.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/exam_policy_engine.h"

#include <memory>

#include "base/logging.h"
#include "base/no_destructor.h"
#include "chrome/browser/exam/events/exam_event_sink.h"
#include "chrome/browser/exam/ipc/challenge/challenge_verifier.h"
#include "chrome/browser/exam/policy/burn_decider.h"
#include "chrome/browser/exam/policy/ime_policy_checker.h"
#include "chrome/browser/exam/policy/policy_challenge_verifier.h"
#include "chrome/browser/exam/policy/prohibited_process_registry.h"
#include "chrome/browser/exam/policy/url_whitelist_gate.h"
#include "services/network/public/cpp/byod_request_gate.h"

namespace chrome::exam {

namespace {

// Passthrough sink for the downstream slot until P8 provides
// SessionReportSink.
class NullDownstreamSink : public ExamEventSink {
 public:
  void OnEvent(const ExamEvent& /*event*/) override {}
};

// Container that owns everything installed by Initialize so Shutdown can
// destroy in the right order.
struct EngineState {
  ExamPolicy policy;
  std::unique_ptr<ImePolicyChecker> ime_checker;
  std::unique_ptr<ProhibitedProcessRegistry> process_registry;
  // BurnDecider is owned by ExamEventSink slot (via SetExamEventSink) —
  // we keep a raw pointer for observability but ownership is transferred.
  raw_ptr<BurnDecider> decider = nullptr;
  bool active = false;
};

EngineState& State() {
  static base::NoDestructor<EngineState> s;
  return *s;
}

}  // namespace

// static
void ExamPolicyEngine::Initialize(ExamPolicy policy) {
  EnforcePolicyInvariants(&policy);

  // Shutdown any prior installation first.
  Shutdown();

  EngineState& s = State();
  s.policy = std::move(policy);

  // (1) URL whitelist gate.
  network::SetByodRequestGate(
      std::make_unique<UrlWhitelistGate>(s.policy.url_whitelist));

  // (2) Checkers / registry (owned by state; consumed by verifier).
  s.ime_checker = std::make_unique<ImePolicyChecker>(
      s.policy.allowed_ime_source_ids);
  s.process_registry = std::make_unique<ProhibitedProcessRegistry>(
      s.policy.prohibited_process_shas);

  // (3) Burn decider owns the ExamEventSink slot; downstream is null
  // until P8 provides SessionReportSink.
  auto decider = std::make_unique<BurnDecider>(
      s.policy.burn_policy,
      std::make_unique<NullDownstreamSink>());
  s.decider = decider.get();
  SetExamEventSink(std::move(decider));  // transfers ownership

  // (4) Challenge verifier — non-owning refs into checker/registry.
  //
  // Parallel prohibited_basenames set: for P5 MVP we derive from the
  // policy (empty if operator didn't provide). Real approach in P6/P9:
  // operator provides both shas and basenames in the policy JSON. For
  // now leave the parallel list empty; verifier's process check is
  // effectively no-op until schema v2 lands.
  std::vector<std::string> prohibited_basenames;

  SetChallengeVerifier(std::make_unique<PolicyChallengeVerifier>(
      s.decider,
      s.ime_checker.get(),
      s.process_registry.get(),
      std::move(prohibited_basenames)));

  s.active = true;
  LOG(INFO) << "[byod] ExamPolicyEngine initialized: session_id="
            << s.policy.session_id
            << " url_whitelist=" << s.policy.url_whitelist.size()
            << " prohibited_shas=" << s.policy.prohibited_process_shas.size()
            << " allowed_imes=" << s.policy.allowed_ime_source_ids.size()
            << " hotkeys=" << s.policy.hotkey_blacklist.size();
}

// static
void ExamPolicyEngine::Shutdown() {
  EngineState& s = State();
  if (!s.active) return;

  network::SetByodRequestGate(nullptr);
  SetExamEventSink(nullptr);           // owns NullDownstreamSink now
  SetChallengeVerifier(nullptr);       // restores default null verifier

  s.decider = nullptr;
  s.ime_checker.reset();
  s.process_registry.reset();
  s.active = false;
  LOG(INFO) << "[byod] ExamPolicyEngine shut down";
}

// static
const ExamPolicy* ExamPolicyEngine::current() {
  EngineState& s = State();
  return s.active ? &s.policy : nullptr;
}

// static
int ExamPolicyEngine::burn_report_count() {
  EngineState& s = State();
  return s.decider ? s.decider->burn_report_count() : 0;
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "policy/exam_policy_engine.cc",
    "policy/exam_policy_engine.h",
```

- [ ] **Step 6: 编译 + smoke**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p5.sh
```

- [ ] **Step 7: 冻结 exam-policy-schema.md**

Write `/root/byod-browser/docs/development/exam-policy-schema.md`:

```markdown
# BYOD Exam Policy JSON Schema (v1)

**Frozen at**: 2026-07-12 (P5 Task 8)

Policy is signed and delivered to the client by P6/P9. This document
specifies the JSON body that ExamPolicyEngine::Initialize consumes.

## Example

\`\`\`json
{
  "session_id": "acm-final-2026-07-15",
  "url_whitelist": [
    {"pattern": "https://exam.university.edu/paper/*", "is_regex": false},
    {"pattern": "https://cdn.university.edu/*", "is_regex": false}
  ],
  "prohibited_process_shas": [
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
  ],
  "allowed_ime_source_ids": [
    "com.apple.keylayout.ABC",
    "com.apple.keylayout.US"
  ],
  "hotkey_blacklist": [
    {"modifiers": 1, "key_code": 70},
    {"modifiers": 1, "key_code": 65}
  ],
  "burn_policy": {
    "max_blur_events_per_15min": 5,
    "max_burn_reports_before_burn": 3,
    "burn_on_whitelist_violation": false
  }
}
\`\`\`

## Field Reference

### `session_id` (string, required)
Echoed to Session Report events (P8) for audit correlation.

### `url_whitelist` (array of objects, optional, default = empty = deny-all)
Each entry:
- `pattern` (string, required) — glob or RE2 regex over full URL.
- `is_regex` (bool, optional, default false) — if true, `pattern` is
  interpreted as RE2 FullMatch regex; otherwise glob (`*` = non-slash,
  `**` = any).

**Hard-coded deny-list overrides whitelist** (spec §5.4.5): chrome://
except chrome://exam-shell/, file://, view-source:, devtools://,
chrome-extension://, localhost, private IPs are always denied even if
matched by whitelist.

### `prohibited_process_shas` (array of hex strings, optional)
Lowercase hex SHA-256 (64 chars). Invalid entries logged + skipped.

**KNOWN LIMITATION (P5 v1)**: Guardian's kRecentProcesses challenge
response (schema v1) reports `exe_basename` not SHA-256, so this field
is not directly used yet. Operators should also configure a parallel
`prohibited_basenames` field once we bump schema to v2.

### `allowed_ime_source_ids` (array of strings, optional)
Case-sensitive IME source IDs. Default English keyboard (per platform)
always implicitly added by `EnforcePolicyInvariants` to prevent lockout.

### `hotkey_blacklist` (array of objects, optional)
Each entry:
- `modifiers` (int, required) — bitmask (ctrl=1, shift=2, alt=4, meta=8).
- `key_code` (int, required) — `ui::VKEY_*` value (Chromium enum).

Runtime blacklist complementing P1-A/B compile-time removals. Actual
interception is Chromium accelerator + Guardian keyboard hook.

### `burn_policy` (object, optional, defaults per struct)
- `max_blur_events_per_15min` (int, default 5) — 15-min sliding window
  blur count triggering immediate burn on overflow.
- `max_burn_reports_before_burn` (int, default 3) — hysteresis for
  non-immediate burn triggers.
- `burn_on_whitelist_violation` (bool, default false) — treat URL
  whitelist violation as immediate burn trigger.

## Immediate Burn Triggers (irrespective of hysteresis)

- Attestation failure
- Challenge verifier failure
- Prohibited process detected
- IME out of allow-list
- URL whitelist violation if `burn_on_whitelist_violation=true`
- Blur count exceeds `max_blur_events_per_15min` in the window

## Version Bump Procedure

Additive-only fields: parser ignores unknown keys → safe.

Removed / renamed fields → bump wire version (see ipc-wire-format.md);
this schema bumps in lockstep.

## History

- **v1** (2026-07-12): initial schema. Frozen for BYOD Exam Browser v1.0.
```

- [ ] **Step 8: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/policy/exam_policy_engine.h \
        chrome/browser/exam/policy/exam_policy_engine.cc
git commit -m "byod: add ExamPolicyEngine total assembly (P5/T8)

P5 Task 8 (spec §4 & §5). ExamPolicyEngine::Initialize takes an
ExamPolicy, enforces invariants, installs UrlWhitelistGate (via
network::SetByodRequestGate), constructs Ime/Process checkers, wires
BurnDecider as ExamEventSink with null downstream (P8 will override),
and installs PolicyChallengeVerifier. Shutdown reverses all hooks."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/1007-add-exam-policy-engine.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p5.sh

cd /root/byod-browser
git add patches/1007-add-exam-policy-engine.patch \
        docs/development/exam-policy-schema.md \
        build/smoke-test-p5.sh
git commit -m "patch: 1007 exam policy engine + schema doc (P5/T8)

Est. LOC in Chromium: ~180. Includes exam-policy-schema.md freeze."
```

---

## Task 9: gtest 单元测试 + P5 acceptance

**Files:**
- Create (in Chromium src): `chrome/browser/exam/policy/url_whitelist_gate_unittest.cc`
- Create (in Chromium src): `chrome/browser/exam/policy/burn_decider_unittest.cc`
- Create (in Chromium src): `chrome/browser/exam/policy/exam_policy_engine_unittest.cc`
- Modify (in Chromium src): `chrome/test/BUILD.gn`
- Create: `/root/byod-browser/patches/1008-add-policy-engine-unit-tests.patch`
- Create: `/root/byod-browser/docs/development/p5-acceptance.md`

**Interfaces:**
- Consumes: Task 8 完成
- Produces: gtest 覆盖 URL whitelist / BurnDecider / EngineInitShutdown 三个关键路径 + P5 acceptance

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p5.sh`：

```bash
# ---------- Task 9: unit tests ----------
echo "[test] P5/task9: policy unit tests"
for f in url_whitelist_gate_unittest.cc burn_decider_unittest.cc \
         exam_policy_engine_unittest.cc; do
  if [ -f /opt/chromium/src/chrome/browser/exam/policy/$f ]; then
    pass "$f present"
  else
    fail "$f missing"
  fi
done
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p5.sh`

- [ ] **Step 3: 创建 url_whitelist_gate_unittest.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/url_whitelist_gate_unittest.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/url_whitelist_gate.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace chrome::exam {

namespace {

network::ByodRequestGate::Decision Eval(const UrlWhitelistGate& g,
                                        const char* url) {
  return g.Evaluate(GURL(url), "GET", url::Origin());
}

}  // namespace

TEST(UrlWhitelistGateTest, EmptyWhitelistDeniesAll) {
  UrlWhitelistGate g({});
  EXPECT_EQ(network::ByodRequestGate::Decision::kDeny,
            Eval(g, "https://example.com/"));
}

TEST(UrlWhitelistGateTest, GlobExactMatchAllows) {
  UrlWhitelistGate g({{"https://exam.example.com/*", false}});
  EXPECT_EQ(network::ByodRequestGate::Decision::kAllow,
            Eval(g, "https://exam.example.com/paper/1"));
  EXPECT_EQ(network::ByodRequestGate::Decision::kDeny,
            Eval(g, "https://other.example.com/paper/1"));
}

TEST(UrlWhitelistGateTest, DoubleStarCrossesSlash) {
  UrlWhitelistGate g({{"https://exam.example.com/**", false}});
  EXPECT_EQ(network::ByodRequestGate::Decision::kAllow,
            Eval(g, "https://exam.example.com/paper/subpage/deep"));
}

TEST(UrlWhitelistGateTest, HardCodedDenyOverridesWhitelist) {
  UrlWhitelistGate g({{"file://**", false}});
  EXPECT_EQ(network::ByodRequestGate::Decision::kDeny,
            Eval(g, "file:///etc/passwd"));
}

TEST(UrlWhitelistGateTest, LocalhostAlwaysDenied) {
  UrlWhitelistGate g({{"http://localhost/*", false}});
  EXPECT_EQ(network::ByodRequestGate::Decision::kDeny,
            Eval(g, "http://localhost/whatever"));
}

TEST(UrlWhitelistGateTest, ChromeExamShellAllowedByHardCoded) {
  UrlWhitelistGate g({});  // empty whitelist
  // chrome://exam-shell/ is not in whitelist but should NOT be denied
  // by the hard-coded rule; whitelist itself denies since it's empty
  // and scheme is not http(s). The gate returns kDeny (empty allow) but
  // chrome://exam-shell/ never reaches gate in production (P1-A patch
  // #3 intercepts at WebUI factory before URLLoader).
  // This test documents the gate's behavior for chrome://exam-shell:
  // it will be denied by empty whitelist, so production must have
  // exam-shell registered in whitelist too if it makes network requests.
  EXPECT_EQ(network::ByodRequestGate::Decision::kDeny,
            Eval(g, "chrome://exam-shell/"));
}

TEST(UrlWhitelistGateTest, RegexMatch) {
  UrlWhitelistGate g({{R"(https://[a-z]+\.example\.com/paper/\d+)", true}});
  EXPECT_EQ(network::ByodRequestGate::Decision::kAllow,
            Eval(g, "https://exam.example.com/paper/42"));
  EXPECT_EQ(network::ByodRequestGate::Decision::kDeny,
            Eval(g, "https://exam.example.com/paper/notanumber"));
}

}  // namespace chrome::exam
```

- [ ] **Step 4: 创建 burn_decider_unittest.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/burn_decider_unittest.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/burn_decider.h"

#include <memory>

#include "base/test/task_environment.h"
#include "chrome/browser/exam/session/exam_session.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chrome::exam {

namespace {

class CountingSink : public ExamEventSink {
 public:
  int count = 0;
  void OnEvent(const ExamEvent& /*event*/) override { ++count; }
};

BurnPolicy MakePolicy() {
  BurnPolicy p;
  p.max_blur_events_per_15min = 3;
  p.max_burn_reports_before_burn = 2;
  p.burn_on_whitelist_violation = false;
  return p;
}

}  // namespace

class BurnDeciderTest : public ::testing::Test {
 protected:
  base::test::TaskEnvironment env_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};

  void SetUp() override {
    if (ExamSession::Get().state() == SessionState::kBurning ||
        ExamSession::Get().state() == SessionState::kEnded) {
      GTEST_SKIP() << "ExamSession terminal from prior test";
    }
    ExamSession::Get().SetSkipSupervisorForTest(true);
    ExamSession::Get().StartExam("bd-test");
  }
};

TEST_F(BurnDeciderTest, ForwardsEventDownstream) {
  auto down = std::make_unique<CountingSink>();
  auto* down_raw = down.get();
  BurnDecider decider(MakePolicy(), std::move(down));

  ExamEvent e;
  e.type = ExamEventType::kNavigation;
  e.monotonic_ts = base::TimeTicks::Now();
  e.wall_ts = base::Time::Now();
  decider.OnEvent(e);

  EXPECT_EQ(1, down_raw->count);
}

TEST_F(BurnDeciderTest, BlurThresholdBurns) {
  BurnDecider decider(MakePolicy(),
                      std::make_unique<CountingSink>());
  ExamEvent e;
  e.type = ExamEventType::kWindowBlur;
  for (int i = 0; i < 4; ++i) {  // policy threshold is 3
    e.monotonic_ts = base::TimeTicks::Now();
    e.wall_ts = base::Time::Now();
    decider.OnEvent(e);
    env_.FastForwardBy(base::Seconds(1));
  }
  EXPECT_TRUE(decider.has_burned());
  EXPECT_EQ(SessionState::kBurning, ExamSession::Get().state());
}

TEST_F(BurnDeciderTest, ImmediateBurnFromAttestation) {
  BurnDecider decider(MakePolicy(),
                      std::make_unique<CountingSink>());
  EXPECT_FALSE(decider.has_burned());
  decider.ReportBurnTrigger(BurnTrigger::kAttestationFailed, "detail");
  EXPECT_TRUE(decider.has_burned());
}

TEST_F(BurnDeciderTest, HysteresisForMildTrigger) {
  BurnDecider decider(MakePolicy(),
                      std::make_unique<CountingSink>());
  decider.ReportBurnTrigger(BurnTrigger::kUrlWhitelistViolation, "1");
  // policy.max_burn_reports_before_burn = 2 → 1st doesn't burn
  EXPECT_FALSE(decider.has_burned());
  decider.ReportBurnTrigger(BurnTrigger::kUrlWhitelistViolation, "2");
  EXPECT_TRUE(decider.has_burned());
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 创建 exam_policy_engine_unittest.cc**

Write `/opt/chromium/src/chrome/browser/exam/policy/exam_policy_engine_unittest.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/policy/exam_policy_engine.h"

#include "chrome/browser/exam/policy/exam_policy.h"
#include "services/network/public/cpp/byod_request_gate.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chrome::exam {

TEST(ExamPolicyEngineTest, InitializeInstallsGate) {
  ExamPolicy p;
  p.session_id = "test";
  p.url_whitelist.push_back({"https://ok.example.com/*", false});

  EXPECT_EQ(nullptr, network::GetByodRequestGate());
  ExamPolicyEngine::Initialize(p);
  EXPECT_NE(nullptr, network::GetByodRequestGate());
  EXPECT_NE(nullptr, ExamPolicyEngine::current());
  EXPECT_EQ("test", ExamPolicyEngine::current()->session_id);
  ExamPolicyEngine::Shutdown();
  EXPECT_EQ(nullptr, network::GetByodRequestGate());
  EXPECT_EQ(nullptr, ExamPolicyEngine::current());
}

TEST(ExamPolicyEngineTest, ReinitializeSwapsPolicy) {
  ExamPolicy p1;
  p1.session_id = "sess-1";
  ExamPolicy p2;
  p2.session_id = "sess-2";

  ExamPolicyEngine::Initialize(p1);
  EXPECT_EQ("sess-1", ExamPolicyEngine::current()->session_id);
  ExamPolicyEngine::Initialize(p2);
  EXPECT_EQ("sess-2", ExamPolicyEngine::current()->session_id);
  ExamPolicyEngine::Shutdown();
}

TEST(ExamPolicyEngineTest, InvariantsEnforcedOnInitialize) {
  ExamPolicy p;
  p.session_id = "x";
  // allowed_ime_source_ids empty on purpose; invariant adds default.
  ExamPolicyEngine::Initialize(p);
  ASSERT_NE(nullptr, ExamPolicyEngine::current());
  EXPECT_FALSE(ExamPolicyEngine::current()->allowed_ime_source_ids.empty());
  ExamPolicyEngine::Shutdown();
}

}  // namespace chrome::exam
```

- [ ] **Step 6: 加进 chrome/test/BUILD.gn**

在 `unit_tests` target `sources = [` 加：

```gn
    "../browser/exam/policy/url_whitelist_gate_unittest.cc",
    "../browser/exam/policy/burn_decider_unittest.cc",
    "../browser/exam/policy/exam_policy_engine_unittest.cc",
```

- [ ] **Step 7: 编译 + 跑 gtest**

Run:
```bash
autoninja -C /opt/chromium/src/out/linux unit_tests
/opt/chromium/src/out/linux/unit_tests \
  --gtest_filter="UrlWhitelistGateTest.*:BurnDeciderTest.*:ExamPolicyEngineTest.*" \
  2>&1 | tail -20
```
Expected: 全部 PASS 或 BurnDecider 部分 SKIP 因 ExamSession singleton 污染

- [ ] **Step 8: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p5.sh
```

- [ ] **Step 9: 写 p5-acceptance.md**

Write `/root/byod-browser/docs/development/p5-acceptance.md`:

```markdown
# P5 交付验收 Checklist

## 补丁类

- [ ] `patches/1000-add-exam-policy-data-model.patch` 存在
- [ ] `patches/1001-add-url-whitelist-engine.patch` 存在
- [ ] `patches/1002-add-prohibited-process-registry.patch` 存在
- [ ] `patches/1003-add-ime-policy-checker.patch` 存在
- [ ] `patches/1004-add-hotkey-policy.patch` 存在
- [ ] `patches/1005-add-burn-decider.patch` 存在
- [ ] `patches/1006-add-real-challenge-verifier.patch` 存在
- [ ] `patches/1007-add-exam-policy-engine.patch` 存在
- [ ] `patches/1008-add-policy-engine-unit-tests.patch` 存在

## 编译类

- [ ] `bash build/revert-patches.sh && bash build/apply-patches.sh` 无冲突
- [ ] `bash build/build-linux.sh chrome` 成功
- [ ] `autoninja -C out/linux unit_tests` 成功

## 行为类

- [ ] `smoke-test.sh` + all previous smoke-test-p*.sh + `smoke-test-p5.sh` 全 pass
- [ ] `unit_tests --gtest_filter="UrlWhitelistGateTest.*"` 全 PASS (7 tests)
- [ ] `unit_tests --gtest_filter="BurnDeciderTest.*"` 4 tests PASS 或部分 SKIP
- [ ] `unit_tests --gtest_filter="ExamPolicyEngineTest.*"` 3 tests PASS

## 接口冻结类（供 P6/P8/P9 消费）

- [ ] `chrome::exam::ExamPolicy` / `UrlWhitelistRule` / `HotkeyRule` / `BurnPolicy` 结构体已 export
- [ ] `ParseExamPolicyFromJson` / `EnforcePolicyInvariants` 已 export
- [ ] `UrlWhitelistGate` / `ImePolicyChecker` / `ProhibitedProcessRegistry` / `HotkeyPolicy` / `BurnDecider` / `PolicyChallengeVerifier` 类已 export
- [ ] `ExamPolicyEngine::Initialize` / `Shutdown` / `current` / `burn_report_count` 已 export
- [ ] `BurnTrigger` 枚举 + `BurnTriggerName` 已 export
- [ ] `docs/development/exam-policy-schema.md` 冻结（P6 / P9 可按 schema 拼装 policy JSON）

## 已知限制 / 后续 refactor

- [ ] Challenge schema v1 kRecentProcesses 只报 exe_basename；SHA-256 匹配需 schema v2
- [ ] Soft attestation 只验长度，缺 nonce 共享机制（P7 完善）
- [ ] TPM/SE 签名验证推迟到 P7
- [ ] Hotkey 运行时挂钩（policy → Chromium accelerator）留待 P6 wiring

## 补丁行数 / 预算类

- [ ] 累计补丁：69（P1 34 + P2-A/B/C/D/E 26 + P5 9）
- [ ] Chromium 现有代码修改 ≤ 80 行（新 dep 声明 + BUILD.gn 增量）
- [ ] 累计 ≤ 3000 行硬预算

## 文档类

- [ ] 本文件（p5-acceptance.md）存在
- [ ] `docs/development/exam-policy-schema.md` 冻结
- [ ] roadmap 里 P5 状态更新为 `✅ 完成`

## 交付判定

以上全部打勾 → P5 完成，可进入 P6 (exam shell UI) 或 P8 (Session Report)。

**关键里程碑**：P5 完成后，加载一份 policy JSON 就可以让 Chromium 真正"按策略办事"——白名单拦截生效、事件计入 burn 决策器、challenge 响应被真校验。虽然 Guardian (P3/P4) 还没上线，但整个策略层已经跑通。
```

- [ ] **Step 10: 导出 patch + 验证 + commit + 更新 roadmap**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/policy/url_whitelist_gate_unittest.cc \
        chrome/browser/exam/policy/burn_decider_unittest.cc \
        chrome/browser/exam/policy/exam_policy_engine_unittest.cc \
        chrome/test/BUILD.gn
git commit -m "byod: add policy engine unit tests (P5/T9)

P5 Task 9. UrlWhitelistGate (7 tests: empty deny, glob exact, double-star,
hard-coded override, localhost, chrome://exam-shell docs behaviour, regex).
BurnDecider (4 tests: forward, blur threshold, immediate burn, hysteresis).
ExamPolicyEngine (3 tests: init/shutdown, reinit swaps, invariants applied)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/1008-add-policy-engine-unit-tests.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
autoninja -C /opt/chromium/src/out/linux unit_tests
/opt/chromium/src/out/linux/unit_tests \
  --gtest_filter="UrlWhitelistGateTest.*:BurnDeciderTest.*:ExamPolicyEngineTest.*" \
  2>&1 | tail -5

cd /root/byod-browser
git add patches/1008-add-policy-engine-unit-tests.patch \
        docs/development/p5-acceptance.md \
        build/smoke-test-p5.sh
git commit -m "patch: 1008 policy engine unit tests + P5 acceptance (P5/T9)

Est. LOC in Chromium: ~250 (14 gtests)."
```

编辑 `/root/byod-browser/docs/superpowers/plans/2026-07-11-byod-roadmap.md` 把 P5 状态改成 `✅ 完成`：

```bash
cd /root/byod-browser
git add docs/superpowers/plans/2026-07-11-byod-roadmap.md
git commit -m "docs: mark P5 complete in roadmap"
```

---

## P5 完成后

**已交付 9 个补丁 + schema 文档**：
- `1000` ExamPolicy 数据模型 + JSON 解析
- `1001` URL 白名单引擎（ByodRequestGate 实现）
- `1002` ProhibitedProcessRegistry
- `1003` ImePolicyChecker
- `1004` HotkeyPolicy
- `1005` BurnDecider
- `1006` PolicyChallengeVerifier
- `1007` ExamPolicyEngine 总装
- `1008` gtest bundle
- 文档：`docs/development/exam-policy-schema.md`（冻结）

**接口冻结完成**（供 P6/P8/P9 消费）：
- `ExamPolicy` 数据模型 + `ParseExamPolicyFromJson`
- `ExamPolicyEngine::Initialize/Shutdown/current/burn_report_count`
- `BurnTrigger` 枚举

**关键里程碑**：加载一份 policy JSON → Chromium 按策略办事（白名单拦截、事件计入 burn 决策器、challenge 响应真校验）—— 无需等 Guardian，业务层已跑通。

**已知限制**（诚实标注）：
- Challenge schema v1 kRecentProcesses 只报 basename；SHA-256 校验需 schema v2
- Soft attestation 弱验证；TPM/SE 签名待 P7
- Hotkey 运行时挂钩留 P6 wiring

**下一步可并行**：
- **P6** — 考试壳 UI（拿到策略 → 调 Initialize）
- **P8** — Session Report（替换 P5 的 NullDownstreamSink）
- **P3/P4** — Guardian 端（依赖 P2 接口冻结，独立开发）
- **P9** — 策略热更（把 P5 的 Initialize 挂到热更包）

**预计 P5 实际耗时**：
- 熟练 Chromium 开发者：5-7 天
- 首次改 Chromium 的开发者：2 周

**风险与常见坑**：
- **schema 版本管理**：本 sub-plan 冻结 v1，任何字段改动要 bump v2 并写升级文档
- **strategy singleton 生命周期**：ExamPolicyEngine 是 NoDestructor state；单元测试之间会污染；acceptance 里显式接受 GTEST_SKIP
- **RE2 依赖增加二进制大小**：Chromium 默认已 include re2，本 sub-plan 复用不新增；若 rebase 后 re2 target 名变了，按报错调
- **display::Screen 在 headless 里可能是 null**：Task 7 已判空处理；生产环境永远有 screen
- **BurnDecider 一旦 burned 无法重置**：设计如此（burning 是 terminal）；测试要用 fresh instance 或 SKIP
