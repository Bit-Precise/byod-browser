# P1-C · 网络与请求扩展补丁 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 合入 2 个网络层扩展补丁：`services/network` 层暴露白名单钩子给 L3（补丁 #8）+ 每请求注入 `X-Exam-*` 签名头（补丁 #9）。覆盖 spec §3.1 补丁 #8、#9 与 spec §5.2 请求头协议。

**Architecture:** 不写业务，只**暴露 hook 点** —— 补丁 #8 在 `URLLoader` 层注入一个 `ByodRequestGate` 接口（decision = allow / deny），deny 时立即 abort request；补丁 #9 在 request headers commit 前注入一个 `ByodHeaderInjector`，把 spec §5.2.2 定义的 4 个 header 写入。两个 hook 的具体实现（白名单规则、session_key 存放、HMAC 计算）**留给 L3（P5）和 Browser Exam Key 模块（P7）**；本 sub-plan 只做接口暴露和 default-null 实现。

**Tech Stack:** Chromium C++ (`services/network`、`net/base`)、`base::NoDestructor` 单例、Mojo IPC 定义、bash smoke tests。

## Global Constraints

- Chromium 补丁总量 ≤ 3000 行（全 P1 累计）；本批预算 ≤ 450 行；P1-A + P1-B + P1-C 累计目标 ≤ 1630 行
- Chromium 补丁只做"删/禁"和"钩子暴露"，**不写业务**（白名单规则、密钥、HMAC 逻辑都在 L3/P7）
- 每个补丁必须独立可 revert；即使 revert 掉 hook 也不影响 Chromium 编译（default null 实现）
- 每个补丁的 commit message 前缀固定 `byod: `；spec 章节引用固定 `see spec §3.1 patch #N`
- 补丁文件命名遵循 P0 patch-workflow.md 的序号分段：`0400-0499` = 网络层扩展
- P1-C 依赖 P1-A + P1-B 已合入（18 个 patch）

---

## File Structure

**本 sub-plan 交付：**

```
/root/byod-browser/
├── patches/
│   ├── 0400-add-byod-request-gate-interface.patch      # 补丁 #8a (接口)
│   ├── 0401-wire-byod-request-gate-into-urlloader.patch # 补丁 #8b (集成)
│   ├── 0402-add-byod-header-injector-interface.patch   # 补丁 #9a (接口)
│   └── 0403-inject-byod-headers-in-network-context.patch # 补丁 #9b (集成)
├── build/
│   └── smoke-test-p1c.sh                               # 本批专用行为验证脚本
└── docs/
    └── development/
        └── p1-c-acceptance.md                          # 本批验收清单
```

**Chromium 源码触及区域：**

- `services/network/public/cpp/byod_request_gate.h` — 新增 header（接口 #8a）
- `services/network/public/cpp/byod_request_gate.cc` — 新增 impl（default null #8a）
- `services/network/url_loader.cc` — 挂钩 (#8b)
- `services/network/public/cpp/byod_header_injector.h` — 新增 header（接口 #9a）
- `services/network/public/cpp/byod_header_injector.cc` — 新增 impl（default null #9a）
- `services/network/network_context.cc` — 挂钩 (#9b)
- `services/network/BUILD.gn` — 加编译单元

**关键跨 sub-plan 接口（P5/P7 依赖）：**

见每个 Task 的 Interfaces 段。P5 会 `SetGate(...)` 注入白名单实现；P7 会 `SetInjector(...)` 注入 Browser Exam Key 计算实现。**接口签名在本 sub-plan 里冻结，后续 sub-plan 只实现不改签名**。

---

## Task 1: 补丁 #8a — 定义 ByodRequestGate 接口

**Files:**
- Create (in Chromium src): `services/network/public/cpp/byod_request_gate.h`
- Create (in Chromium src): `services/network/public/cpp/byod_request_gate.cc`
- Modify (in Chromium src): `services/network/public/cpp/BUILD.gn` — 加编译单元
- Create: `/root/byod-browser/patches/0400-add-byod-request-gate-interface.patch`
- Create: `/root/byod-browser/build/smoke-test-p1c.sh`

**Interfaces:**
- Consumes: P1-B 全部完成（18 个 patch 已合入）
- **Produces**（P5 消费）:
  ```cpp
  // In namespace network:
  class ByodRequestGate {
   public:
    // Called on the network service thread for every outgoing request.
    // Return kAllow to permit, kDeny to abort with net::ERR_BLOCKED_BY_CLIENT.
    // Must be thread-safe and non-blocking.
    enum class Decision { kAllow, kDeny };
    virtual Decision Evaluate(const GURL& url,
                              const std::string& method,
                              const url::Origin& initiator) const = 0;
    virtual ~ByodRequestGate() = default;
  };

  // Global setter/getter. Default is null → all requests allowed (vanilla
  // behavior). L3 (P5) calls SetGate(std::make_unique<ExamWhitelistGate>(...)).
  void SetByodRequestGate(std::unique_ptr<ByodRequestGate> gate);
  const ByodRequestGate* GetByodRequestGate();
  ```

- [ ] **Step 1: 起始状态自检**

Run:
```bash
cd /opt/chromium/src
git status
git describe --tags
git log --oneline | head -20
```
Expected: `working tree clean`；tag 与 `chromium-version.env` 的 `CHROMIUM_TAG` 一致；最近 18 条 commit 是 P1-A + P1-B 的补丁（消息以 `byod:` 开头）。

若不干净：`echo y | bash /root/byod-browser/build/revert-patches.sh && bash /root/byod-browser/build/apply-patches.sh`

- [ ] **Step 2: 定位 BUILD.gn source list**

Run:
```bash
grep -n "sources = " /opt/chromium/src/services/network/public/cpp/BUILD.gn | head -20
```
Expected: 一处 `sources = [` 后跟若干 header/cc 文件；记录行号，本任务的新文件会加进去。

- [ ] **Step 3: 写 smoke-test-p1c.sh 骨架 + 补丁 #8a 测试段**

Write `/root/byod-browser/build/smoke-test-p1c.sh`:

```bash
#!/usr/bin/env bash
# Behavior smoke tests for P1-C patches (#8, #9).
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

# ---------- Patch #8a: gate symbols exist ----------
echo "[test] patch #8a: ByodRequestGate symbols compiled in"

# We can't easily inspect linked symbols in a stripped release build, but
# we can verify the header file exists (via file listing on disk) —
# indirect confirmation. Direct behavior test comes with #8b.
if [ -f /opt/chromium/src/services/network/public/cpp/byod_request_gate.h ]; then
  pass "#8a: byod_request_gate.h present in tree"
else
  fail "#8a: byod_request_gate.h missing"
fi

# Verify chrome still runs (interface change must not break vanilla)
out="$(run_chrome --headless --dump-dom https://example.com 2>&1 || true)"
if echo "$out" | grep -q "Example Domain"; then
  pass "#8a: vanilla request path still works (default null gate = allow-all)"
else
  fail "#8a: example.com no longer loads after gate wiring"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "[fail] one or more P1-C tests failed"; exit 1
fi
echo "[success] all P1-C tests passed"
```

Run:
```bash
chmod +x /root/byod-browser/build/smoke-test-p1c.sh
```

- [ ] **Step 4: 跑一次确认现在失败**

Run: `bash /root/byod-browser/build/smoke-test-p1c.sh`
Expected: `✗ #8a: byod_request_gate.h missing`

- [ ] **Step 5: 创建 header 文件**

Write `/opt/chromium/src/services/network/public/cpp/byod_request_gate.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #8a (spec §3.1, §4.1.1): hook point for the L3 policy engine
// to gate outgoing network requests against the exam whitelist. The default
// implementation is null (allow-all); L3 replaces it at exam-shell startup
// via SetByodRequestGate().

#ifndef SERVICES_NETWORK_PUBLIC_CPP_BYOD_REQUEST_GATE_H_
#define SERVICES_NETWORK_PUBLIC_CPP_BYOD_REQUEST_GATE_H_

#include <memory>
#include <string>

#include "base/component_export.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace network {

class COMPONENT_EXPORT(NETWORK_CPP) ByodRequestGate {
 public:
  enum class Decision { kAllow, kDeny };

  // Called on the network service thread. Must be thread-safe, non-blocking,
  // and return promptly (< 1ms for the common allow path).
  virtual Decision Evaluate(const GURL& url,
                            const std::string& method,
                            const url::Origin& initiator) const = 0;

  virtual ~ByodRequestGate() = default;
};

// Install a gate implementation. Passing nullptr restores default (allow-all).
// Called from the browser process only, before any URLLoader is created.
COMPONENT_EXPORT(NETWORK_CPP)
void SetByodRequestGate(std::unique_ptr<ByodRequestGate> gate);

// Get the currently installed gate, or nullptr if none.
COMPONENT_EXPORT(NETWORK_CPP)
const ByodRequestGate* GetByodRequestGate();

}  // namespace network

#endif  // SERVICES_NETWORK_PUBLIC_CPP_BYOD_REQUEST_GATE_H_
```

- [ ] **Step 6: 创建 impl 文件**

Write `/opt/chromium/src/services/network/public/cpp/byod_request_gate.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "services/network/public/cpp/byod_request_gate.h"

#include "base/no_destructor.h"
#include "base/synchronization/lock.h"

namespace network {

namespace {

// Thread-safe holder. SetByodRequestGate is called once from the browser
// process at startup; Evaluate() is called from many network threads.
struct GateHolder {
  mutable base::Lock lock;
  std::unique_ptr<ByodRequestGate> gate;  // GUARDED_BY(lock)
};

GateHolder& GetHolder() {
  static base::NoDestructor<GateHolder> holder;
  return *holder;
}

}  // namespace

void SetByodRequestGate(std::unique_ptr<ByodRequestGate> gate) {
  GateHolder& holder = GetHolder();
  base::AutoLock auto_lock(holder.lock);
  holder.gate = std::move(gate);
}

const ByodRequestGate* GetByodRequestGate() {
  GateHolder& holder = GetHolder();
  base::AutoLock auto_lock(holder.lock);
  return holder.gate.get();
}

}  // namespace network
```

- [ ] **Step 7: 修改 BUILD.gn 加入新文件**

在 `/opt/chromium/src/services/network/public/cpp/BUILD.gn` 里，找到 Step 2 定位的主 source list（通常是 `component("cpp")` 或 `jumbo_component("cpp")` target 的 `sources = [ ... ]`），在 alphabetical 位置加入：

```gn
    "byod_request_gate.cc",
    "byod_request_gate.h",
```

（如果 `sources` 已按字母排序，插入到 `binary_writer.*` 之类的邻居间；不严格排序则末尾追加也可）

- [ ] **Step 8: 编译**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
```
Expected: 增量编译，成功。**若报"COMPONENT_EXPORT 未定义"**：确认 include `base/component_export.h`。**若报 "NETWORK_CPP 未定义"**：查看邻近 header 使用的 export macro 名，可能是 `COMPONENT_EXPORT(NETWORK_CPP_BASE)`；改成实际使用的名字。

- [ ] **Step 9: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
```
Expected: 全部 pass；`#8a` 显示 `✓ byod_request_gate.h present` 和 `✓ vanilla request path still works`

- [ ] **Step 10: Chromium 端 commit + 导出 patch**

Run:
```bash
cd /opt/chromium/src
git add services/network/public/cpp/byod_request_gate.h \
        services/network/public/cpp/byod_request_gate.cc \
        services/network/public/cpp/BUILD.gn
git commit -m "byod: add ByodRequestGate interface (patch #8a)

Patch #8a (spec §3.1, §4.1.1). Public C++ interface for the L3 policy
engine (P5) to gate outgoing network requests. Default is null
(allow-all). Wired into URLLoader by #8b."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0400-add-byod-request-gate-interface.patch
```

- [ ] **Step 11: Revert + re-apply 循环验证**

Run:
```bash
echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1c.sh
```
Expected: 全成功

- [ ] **Step 12: Commit patch 到本 repo**

Run:
```bash
cd /root/byod-browser
git add patches/0400-add-byod-request-gate-interface.patch build/smoke-test-p1c.sh
git commit -m "patch: 0400 ByodRequestGate interface (patch #8a)

Est. LOC: ~90 (~60 lines .h + .cc + 2 lines BUILD.gn)."
```

---

## Task 2: 补丁 #8b — 把 ByodRequestGate 挂到 URLLoader

**Files:**
- Modify (in Chromium src): `services/network/url_loader.cc`
- Create: `/root/byod-browser/patches/0401-wire-byod-request-gate-into-urlloader.patch`

**Interfaces:**
- Consumes: Task 1 完成（`byod_request_gate.h` 可 include）
- Produces: 每个 outgoing HTTP(S) 请求在 URLLoader 层调用 `GetByodRequestGate()->Evaluate(...)`；返回 `kDeny` 则 request 立即失败 with `net::ERR_BLOCKED_BY_CLIENT`

- [ ] **Step 1: 定位 URLLoader 请求启动点**

Run:
```bash
grep -n "^void URLLoader::URLLoader\|void URLLoader::Start\|URLLoader::StartRequest\|URLLoader::ScheduleStart" /opt/chromium/src/services/network/url_loader.cc | head -10
grep -n "NotifyCompleted\|ERR_BLOCKED_BY_CLIENT" /opt/chromium/src/services/network/url_loader.cc | head -10
```
Expected: `URLLoader::StartRequest` 或类似函数所在行；`NotifyCompleted` 是完成回调所在行。

- [ ] **Step 2: 追加 smoke test #8b**

Edit `/root/byod-browser/build/smoke-test-p1c.sh`，在 `# ---------- Patch #8a` 段之后追加：

```bash
# ---------- Patch #8b: gate called by URLLoader (default null = allow-all) ----------
echo "[test] patch #8b: URLLoader wired to gate"

# With default null gate, allow-all behavior means example.com still loads.
# (Real deny behavior tested in P5 when a whitelist gate is installed.)
out="$(run_chrome --headless --dump-dom https://example.com 2>&1 || true)"
if echo "$out" | grep -q "Example Domain"; then
  pass "#8b: default null gate → allow-all (baseline preserved)"
else
  fail "#8b: URLLoader path broken after gate wiring"
fi
```

- [ ] **Step 3: 跑 smoke 确认（应该已通过 —— 因为 gate 还没接入所以 baseline 是原样）**

Run: `bash /root/byod-browser/build/smoke-test-p1c.sh`
Expected: `✓ #8b: default null gate → allow-all` —— 实际这只是 baseline check；真正测 "wired" 会在 Step 5 后重新有意义。

- [ ] **Step 4: 修改 url_loader.cc — include header + 调用 gate**

在 `/opt/chromium/src/services/network/url_loader.cc` 顶部 include 区域添加：

```cpp
#include "services/network/public/cpp/byod_request_gate.h"
```

在 `URLLoader::StartRequest`（或 grep 定位到的实际请求启动函数）函数体入口，在实际发出请求（`net_request_->Start()` 或类似调用）**之前**添加：

```cpp
  // BYOD patch #8b (spec §3.1, §4.1.1): consult the exam whitelist gate
  // before sending the request. Default null gate = allow-all (vanilla
  // Chromium behavior). L3 (P5) installs an ExamWhitelistGate at exam
  // startup that returns kDeny for non-whitelisted URLs.
  {
    const ByodRequestGate* gate = GetByodRequestGate();
    if (gate) {
      const GURL& url = url_request_->url();
      const std::string& method = url_request_->method();
      url::Origin initiator =
          url_request_->initiator().value_or(url::Origin());
      if (gate->Evaluate(url, method, initiator) ==
          ByodRequestGate::Decision::kDeny) {
        NotifyCompleted(net::ERR_BLOCKED_BY_CLIENT);
        return;
      }
    }
  }
```

**注意 API 命名差异**：
- 若 URLLoader 里的 request 成员叫 `url_request_` 之外的名字（`net_request_`、`request_`），改用实际的
- `url_request_->initiator()` 返回值类型在 Chromium 版本间可能变化，若报 `no member "value_or"` 则改用 `absl::optional<url::Origin>` 的显式判断
- `NotifyCompleted` 若签名变化，检索合适的完成回调函数（`OnComplete`、`SendCompletionAndDeleteIfDone` 等）

- [ ] **Step 5: 增量编译**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
```
Expected: 增量编译（改动集中在 network 服务），10-20 分钟。

- [ ] **Step 6: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
```
Expected: 所有测试通过（因为 default gate 仍是 null，行为不变）

- [ ] **Step 7: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add services/network/url_loader.cc
git commit -m "byod: wire ByodRequestGate into URLLoader (patch #8b)

Patch #8b (spec §3.1, §4.1.1). Every outgoing request consults the
globally-installed gate. Default null gate preserves vanilla allow-all
behavior; L3 (P5) will install ExamWhitelistGate."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0401-wire-byod-request-gate-into-urlloader.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1c.sh

cd /root/byod-browser
git add patches/0401-wire-byod-request-gate-into-urlloader.patch build/smoke-test-p1c.sh
git commit -m "patch: 0401 wire gate into URLLoader (patch #8b)

Est. LOC: ~25."
```

---

## Task 3: 补丁 #9a — 定义 ByodHeaderInjector 接口

**Files:**
- Create (in Chromium src): `services/network/public/cpp/byod_header_injector.h`
- Create (in Chromium src): `services/network/public/cpp/byod_header_injector.cc`
- Modify (in Chromium src): `services/network/public/cpp/BUILD.gn` — 加编译单元
- Create: `/root/byod-browser/patches/0402-add-byod-header-injector-interface.patch`

**Interfaces:**
- Consumes: Task 2 完成
- **Produces**（P7 消费）:
  ```cpp
  namespace network {

  struct COMPONENT_EXPORT(NETWORK_CPP) ByodExamHeaders {
    std::string auth;         // "X-Exam-Auth"
    std::string timestamp;    // "X-Exam-Timestamp" (ISO 8601)
    std::string nonce;        // "X-Exam-Nonce" (base64)
    std::string fingerprint;  // "X-Exam-Client-Fingerprint"
  };

  class COMPONENT_EXPORT(NETWORK_CPP) ByodHeaderInjector {
   public:
    // Called on network service thread for every outgoing request that
    // passed the gate. Return std::nullopt to skip injection.
    virtual std::optional<ByodExamHeaders> ComputeHeaders(
        const GURL& url,
        const std::string& method) const = 0;
    virtual ~ByodHeaderInjector() = default;
  };

  COMPONENT_EXPORT(NETWORK_CPP)
  void SetByodHeaderInjector(std::unique_ptr<ByodHeaderInjector> injector);
  COMPONENT_EXPORT(NETWORK_CPP)
  const ByodHeaderInjector* GetByodHeaderInjector();

  }  // namespace network
  ```

- [ ] **Step 1: 追加 smoke test #9a**

Edit `/root/byod-browser/build/smoke-test-p1c.sh`:

```bash
# ---------- Patch #9a: header injector symbols exist ----------
echo "[test] patch #9a: ByodHeaderInjector symbols compiled in"

if [ -f /opt/chromium/src/services/network/public/cpp/byod_header_injector.h ]; then
  pass "#9a: byod_header_injector.h present"
else
  fail "#9a: byod_header_injector.h missing"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1c.sh`
Expected: `✗ #9a: byod_header_injector.h missing`

- [ ] **Step 3: 创建 header 文件**

Write `/opt/chromium/src/services/network/public/cpp/byod_header_injector.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #9a (spec §3.1, §5.2.2): hook point for the Browser Exam Key
// module (P7) to inject signed headers on every outgoing exam-domain
// request. Default is null (no headers injected).

#ifndef SERVICES_NETWORK_PUBLIC_CPP_BYOD_HEADER_INJECTOR_H_
#define SERVICES_NETWORK_PUBLIC_CPP_BYOD_HEADER_INJECTOR_H_

#include <memory>
#include <optional>
#include <string>

#include "base/component_export.h"
#include "url/gurl.h"

namespace network {

// Set of headers computed per request by the Browser Exam Key module.
// Empty strings for any field mean "do not add this header".
struct COMPONENT_EXPORT(NETWORK_CPP) ByodExamHeaders {
  // HMAC-SHA256(session_key, method || url_path || iso_timestamp || nonce)
  // base64-encoded. Sent as X-Exam-Auth.
  std::string auth;
  // ISO 8601, e.g. "2026-07-12T14:30:22Z". Sent as X-Exam-Timestamp.
  std::string timestamp;
  // Base64 of 16 random bytes. Sent as X-Exam-Nonce.
  std::string nonce;
  // Hardware-attested fingerprint (see spec §5.2.3). Sent as
  // X-Exam-Client-Fingerprint.
  std::string fingerprint;
};

class COMPONENT_EXPORT(NETWORK_CPP) ByodHeaderInjector {
 public:
  // Return std::nullopt to skip injection for this request (e.g. non-exam
  // domains). Must be thread-safe and non-blocking.
  virtual std::optional<ByodExamHeaders> ComputeHeaders(
      const GURL& url,
      const std::string& method) const = 0;

  virtual ~ByodHeaderInjector() = default;
};

// Install an injector. Passing nullptr restores default (no injection).
COMPONENT_EXPORT(NETWORK_CPP)
void SetByodHeaderInjector(std::unique_ptr<ByodHeaderInjector> injector);

COMPONENT_EXPORT(NETWORK_CPP)
const ByodHeaderInjector* GetByodHeaderInjector();

}  // namespace network

#endif  // SERVICES_NETWORK_PUBLIC_CPP_BYOD_HEADER_INJECTOR_H_
```

- [ ] **Step 4: 创建 impl 文件**

Write `/opt/chromium/src/services/network/public/cpp/byod_header_injector.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "services/network/public/cpp/byod_header_injector.h"

#include "base/no_destructor.h"
#include "base/synchronization/lock.h"

namespace network {

namespace {

struct InjectorHolder {
  mutable base::Lock lock;
  std::unique_ptr<ByodHeaderInjector> injector;  // GUARDED_BY(lock)
};

InjectorHolder& GetHolder() {
  static base::NoDestructor<InjectorHolder> holder;
  return *holder;
}

}  // namespace

void SetByodHeaderInjector(std::unique_ptr<ByodHeaderInjector> injector) {
  InjectorHolder& holder = GetHolder();
  base::AutoLock auto_lock(holder.lock);
  holder.injector = std::move(injector);
}

const ByodHeaderInjector* GetByodHeaderInjector() {
  InjectorHolder& holder = GetHolder();
  base::AutoLock auto_lock(holder.lock);
  return holder.injector.get();
}

}  // namespace network
```

- [ ] **Step 5: 修改 BUILD.gn 加入新文件**

在同一 `sources = [` 列表里加入：

```gn
    "byod_header_injector.cc",
    "byod_header_injector.h",
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
```

- [ ] **Step 8: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add services/network/public/cpp/byod_header_injector.h \
        services/network/public/cpp/byod_header_injector.cc \
        services/network/public/cpp/BUILD.gn
git commit -m "byod: add ByodHeaderInjector interface (patch #9a)

Patch #9a (spec §3.1, §5.2.2). Interface for Browser Exam Key module
(P7) to inject X-Exam-* signed headers on outgoing requests. Default
null = no injection."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0402-add-byod-header-injector-interface.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1c.sh

cd /root/byod-browser
git add patches/0402-add-byod-header-injector-interface.patch build/smoke-test-p1c.sh
git commit -m "patch: 0402 ByodHeaderInjector interface (patch #9a)

Est. LOC: ~100."
```

---

## Task 4: 补丁 #9b — 把 ByodHeaderInjector 挂到 URLLoader（headers commit 前）

**Files:**
- Modify (in Chromium src): `services/network/url_loader.cc`（同 Task 2 的文件，追加 include + 追加代码块）
- Create: `/root/byod-browser/patches/0403-inject-byod-headers-in-network-context.patch`

**Interfaces:**
- Consumes: Task 3 完成
- Produces: 每个 outgoing 请求在 headers 最终 commit 前调用 `GetByodHeaderInjector()->ComputeHeaders(...)`；返回的 headers 用 `SetHeader` 覆盖同名 header（若原本存在），或新增

**说明**：本 patch 与 #8b 在同一函数里（`URLLoader::StartRequest`），两次修改文件 → 分成两个独立 patch commit，方便未来单独 revert。

- [ ] **Step 1: 追加 smoke test #9b**

Edit `/root/byod-browser/build/smoke-test-p1c.sh`:

```bash
# ---------- Patch #9b: header injector wired (default null = no injection) ----------
echo "[test] patch #9b: URLLoader wired to header injector"

# With default null injector, no X-Exam-* headers should appear.
# We use a local httpbin-like echo server... but headless Chromium doesn't
# expose request headers directly. Instead we verify vanilla behavior is
# preserved (loading example.com still works).
out="$(run_chrome --headless --dump-dom https://example.com 2>&1 || true)"
if echo "$out" | grep -q "Example Domain"; then
  pass "#9b: default null injector → no headers, baseline preserved"
else
  fail "#9b: URLLoader path broken after injector wiring"
fi

# The real header-injection behavior is tested in P7 with a full injector
# implementation. Here we only verify the hook doesn't break vanilla path.
```

- [ ] **Step 2: 修改 url_loader.cc — include header + 调用 injector**

在 `/opt/chromium/src/services/network/url_loader.cc` 顶部 include 区域追加（已经有 #8b 的 include 一起）：

```cpp
#include "services/network/public/cpp/byod_header_injector.h"
```

在 `URLLoader::StartRequest` 函数里，**在 #8b 的 gate 检查之后**（gate 允许通过后，实际发出请求之前）添加：

```cpp
  // BYOD patch #9b (spec §3.1, §5.2.2): inject signed exam headers.
  // Default null injector = no headers added (vanilla behavior preserved).
  // P7 installs a BrowserExamKeyInjector that computes HMAC-SHA256 auth
  // headers using the exam session key.
  {
    const ByodHeaderInjector* injector = GetByodHeaderInjector();
    if (injector) {
      const GURL& url = url_request_->url();
      const std::string& method = url_request_->method();
      std::optional<ByodExamHeaders> headers =
          injector->ComputeHeaders(url, method);
      if (headers.has_value()) {
        net::HttpRequestHeaders extra;
        if (!headers->auth.empty())
          extra.SetHeader("X-Exam-Auth", headers->auth);
        if (!headers->timestamp.empty())
          extra.SetHeader("X-Exam-Timestamp", headers->timestamp);
        if (!headers->nonce.empty())
          extra.SetHeader("X-Exam-Nonce", headers->nonce);
        if (!headers->fingerprint.empty())
          extra.SetHeader("X-Exam-Client-Fingerprint", headers->fingerprint);
        url_request_->SetExtraRequestHeaders(extra);
      }
    }
  }
```

**注意**：
- `SetExtraRequestHeaders` 会**覆盖**当前 extra headers。如果 URLLoader 已经通过其他路径设置了 extra headers，我们的调用会覆盖它。为兼容起见，改成先 `net::HttpRequestHeaders existing = url_request_->extra_request_headers();`，再 `existing.MergeFrom(extra); url_request_->SetExtraRequestHeaders(existing);`。**若 URLRequest API 提供 `AddExtraHeader` 直接追加**（多数版本都有 `SetExtraRequestHeaders(net::HttpRequestHeaders)`，无 `Add`），走 merge 路径。
- 若在 request 已 Start 后修改 headers 会 CHECK-fail：确保插入位置在 `url_request_->Start()` 之前（如果 URLLoader 里 Start 已在别的函数调用，把插入位置挪到那之前）

- [ ] **Step 3: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**若报"HttpRequestHeaders 未定义"**：add `#include "net/http/http_request_headers.h"`。

- [ ] **Step 4: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
```

- [ ] **Step 5: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add services/network/url_loader.cc
git commit -m "byod: inject Browser Exam Key headers in URLLoader (patch #9b)

Patch #9b (spec §3.1, §5.2.2). X-Exam-Auth / X-Exam-Timestamp /
X-Exam-Nonce / X-Exam-Client-Fingerprint injected by installed
ByodHeaderInjector (P7). Default null injector = no headers."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0403-inject-byod-headers-in-network-context.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1c.sh

cd /root/byod-browser
git add patches/0403-inject-byod-headers-in-network-context.patch build/smoke-test-p1c.sh
git commit -m "patch: 0403 wire header injector into URLLoader (patch #9b)

Est. LOC: ~30."
```

---

## Task 5: 端到端集成测试 — 用测试 gate + 测试 injector 验证行为

**Files:**
- Create (in Chromium src, temporary — deleted at end of task): `services/network/byod_test_helpers.cc`
- Modify (in Chromium src, temporary): `chrome/browser/chrome_browser_main.cc` — 加载测试 gate/injector
- **不导出 patch** — 这是纯本地行为验证，不写入 `patches/`

**Interfaces:**
- Consumes: Task 4 完成
- Produces: 一次性行为验证 —— 证明 gate 能 deny、injector 能加 header；结束后 revert 掉所有测试代码

**说明**：这个 task 是**验证性质**，不产出 patch —— 我们只想眼见为实"接口真的工作"。P5/P7 会用真正的实现替代。

- [ ] **Step 1: 写一个测试 gate + injector**

Write `/opt/chromium/src/services/network/byod_test_helpers.cc`（临时文件）:

```cpp
// TEMPORARY: BYOD P1-C Task 5 verification. Delete before commit.
// Registers a test gate that denies example.com and a test injector that
// stamps fixed X-Exam-* headers on every request.

#include "services/network/public/cpp/byod_request_gate.h"
#include "services/network/public/cpp/byod_header_injector.h"

namespace network {
namespace {

class TestGate : public ByodRequestGate {
 public:
  Decision Evaluate(const GURL& url, const std::string&,
                    const url::Origin&) const override {
    if (url.host().find("example.com") != std::string::npos)
      return Decision::kDeny;
    return Decision::kAllow;
  }
};

class TestInjector : public ByodHeaderInjector {
 public:
  std::optional<ByodExamHeaders> ComputeHeaders(
      const GURL& url, const std::string&) const override {
    if (url.host().find("httpbin.org") == std::string::npos)
      return std::nullopt;
    ByodExamHeaders h;
    h.auth = "TEST_AUTH_TOKEN";
    h.timestamp = "2026-07-12T00:00:00Z";
    h.nonce = "TEST_NONCE";
    h.fingerprint = "TEST_FINGERPRINT";
    return h;
  }
};

}  // namespace

void InstallByodTestHelpers() {
  SetByodRequestGate(std::make_unique<TestGate>());
  SetByodHeaderInjector(std::make_unique<TestInjector>());
}

}  // namespace network
```

- [ ] **Step 2: 临时把 helpers 挂到 browser main**

在 `/opt/chromium/src/chrome/browser/chrome_browser_main.cc` 里找到 `ChromeBrowserMainParts::PreMainMessageLoopRun()` 或类似启动点，在函数体开头添加（**记住位置，稍后要删**）：

```cpp
  // TEMP P1-C Task 5 verification — DELETE after test.
  extern void InstallByodTestHelpers();
  InstallByodTestHelpers();
```

- [ ] **Step 3: 加进 BUILD.gn（临时）**

在 `/opt/chromium/src/services/network/BUILD.gn` 找到 network service 主 target 的 sources list，临时加入：

```gn
    "byod_test_helpers.cc",
```

（这一步是为了让 `InstallByodTestHelpers()` 符号能被 browser main link 到）

- [ ] **Step 4: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

- [ ] **Step 5: 验证 gate deny 生效**

Run:
```bash
out="$(/opt/chromium/src/out/linux/chrome \
  --user-data-dir=/tmp/byod-p1c-t5 \
  --no-first-run --no-sandbox --disable-gpu \
  --headless --dump-dom https://example.com 2>&1 || true)"
echo "$out" | head -20
```
Expected: **不返回** `Example Domain`；应该看到 network error 或空 body（因 gate 返回 kDeny → `net::ERR_BLOCKED_BY_CLIENT`）

- [ ] **Step 6: 验证 injector 加 header 生效**

Run:
```bash
out="$(/opt/chromium/src/out/linux/chrome \
  --user-data-dir=/tmp/byod-p1c-t5-hdr \
  --no-first-run --no-sandbox --disable-gpu \
  --headless --dump-dom https://httpbin.org/headers 2>&1 || true)"
echo "$out" | grep -E "X-Exam-Auth|X-Exam-Timestamp|X-Exam-Nonce|X-Exam-Client-Fingerprint"
```
Expected: 至少 3-4 行输出，形如 `"X-Exam-Auth": "TEST_AUTH_TOKEN"` 等 —— httpbin.org/headers 会把收到的所有 headers echo 回来

**若 httpbin.org 网络不可达**：改用本地起一个 echo server：

```bash
python3 -m http.server 8080 --directory /tmp &
# 然后访问 http://localhost:8080 —— 但要绕过 hard-coded 最小信任规则（本机 IP 禁），需要临时用 https 公网服务
```

**接受此局限**：若 httpbin 不可达，跳过 Step 6，把它记录到 acceptance checklist 的"可选验证"里。

- [ ] **Step 7: 移除临时代码（全部 revert）**

Run:
```bash
cd /opt/chromium/src
rm services/network/byod_test_helpers.cc
git checkout chrome/browser/chrome_browser_main.cc
git checkout services/network/BUILD.gn
# 或整体 revert：如果没 add，直接 checkout 即可

# 确认干净
git status
```
Expected: `working tree clean`

- [ ] **Step 8: 再编一次确认没残留**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
```
Expected: 全通过（且现在 `example.com` 又能加载了，因为 gate 已 revert）

**本 task 不做 git commit**（没有产出 patch）；只是行为验证。

---

## Task 6: 补丁总量预算校验 + 交付确认

**Files:**
- Create: `/root/byod-browser/docs/development/p1-c-acceptance.md`

**Interfaces:**
- Consumes: Task 1..5 全部完成
- Produces: P1-C 交付验收 checklist

- [ ] **Step 1: 统计当前 patch 总行数**

Run:
```bash
source /root/byod-browser/build/chromium-version.env
cd /opt/chromium/src
git diff --shortstat "$CHROMIUM_TAG"..HEAD
```
Expected: P1-A + P1-B + P1-C 累计 insertions ≤ 1630

- [ ] **Step 2: 数累计补丁文件数**

Run:
```bash
ls /root/byod-browser/patches/*.patch | wc -l
```
Expected: 22（P1-A 9 + P1-B 9 + P1-C 4）

- [ ] **Step 3: 验证所有 patch 都能干净应用 + 全量 smoke**

Run:
```bash
echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
```
Expected: 全部成功

- [ ] **Step 4: 写 acceptance checklist**

Write `/root/byod-browser/docs/development/p1-c-acceptance.md`:

```markdown
# P1-C 交付验收 Checklist

## 补丁类

- [ ] `patches/0400-add-byod-request-gate-interface.patch` 存在
- [ ] `patches/0401-wire-byod-request-gate-into-urlloader.patch` 存在
- [ ] `patches/0402-add-byod-header-injector-interface.patch` 存在
- [ ] `patches/0403-inject-byod-headers-in-network-context.patch` 存在

## 编译类

- [ ] `bash build/revert-patches.sh && bash build/apply-patches.sh` 无冲突
- [ ] `bash build/build-linux.sh chrome` 成功
- [ ] `git diff --shortstat "$CHROMIUM_TAG"..HEAD` 显示 insertions ≤ 1630（P1-A + P1-B + P1-C 累计）

## 行为类（Linux headless smoke tests）

- [ ] `bash build/smoke-test.sh` 全部 pass
- [ ] `bash build/smoke-test-p1a.sh` 全部 pass
- [ ] `bash build/smoke-test-p1b.sh` 全部 pass
- [ ] `bash build/smoke-test-p1c.sh` 全部 pass

## 端到端集成类（Task 5）

- [ ] 装上 TestGate（deny example.com）后，访问 https://example.com 返回 network error 而非页面内容
- [ ] 装上 TestInjector（stamp fixed headers）后，访问 httpbin.org/headers 能看到 `X-Exam-Auth` / `X-Exam-Timestamp` / `X-Exam-Nonce` / `X-Exam-Client-Fingerprint` 4 个 headers 都出现
- [ ] Task 5 结束后 test helpers 全部 revert，`git status` 干净

## 接口冻结类（供 P5/P7 消费）

- [ ] `services/network/public/cpp/byod_request_gate.h` 的 `SetByodRequestGate` / `GetByodRequestGate` / `ByodRequestGate::Evaluate` / `Decision::kAllow/kDeny` 已 export
- [ ] `services/network/public/cpp/byod_header_injector.h` 的 `SetByodHeaderInjector` / `GetByodHeaderInjector` / `ByodHeaderInjector::ComputeHeaders` / `ByodExamHeaders` 结构体已 export
- [ ] 这两个 header 已被 P5 (policy engine) 与 P7 (browser exam key) 的 sub-plan 引用

## 文档类

- [ ] 本文件（p1-c-acceptance.md）存在
- [ ] `docs/superpowers/plans/2026-07-11-byod-roadmap.md` 里 P1 状态更新为 `⏳ 进行中 (P1-A + P1-B + P1-C 完成)`

## 交付判定

以上全部打勾 → P1-C 完成，可进入 P1-D（尚未编写）。
```

- [ ] **Step 5: Commit acceptance + 更新 roadmap**

Run:
```bash
cd /root/byod-browser
git add docs/development/p1-c-acceptance.md
git commit -m "docs: add P1-C acceptance checklist"
```

编辑 `/root/byod-browser/docs/superpowers/plans/2026-07-11-byod-roadmap.md` 把 P1 行状态改成 `⏳ 进行中 (P1-A + P1-B + P1-C 完成)`，然后：

```bash
cd /root/byod-browser
git add docs/superpowers/plans/2026-07-11-byod-roadmap.md
git commit -m "docs: mark P1-C complete in roadmap"
```

---

## P1-C 完成后

- 已交付 4 个补丁文件（对应 spec §3.1 补丁 #8、#9 各拆细为接口 + 集成两部分）
- 累计 patch 行数应 ≤ 1630（P1-A 600 + P1-B 580 + P1-C 450 预算）
- 下一批：**P1-D**（补丁 #12 IPC 握手、#13 Session 生命周期、#14 事件采集 —— 全 P1 最复杂的一批），预算 ~900 行

**预计实际耗时**（含每次增量编译）：
- 熟练 Chromium 开发者：2-3 天
- 首次改 Chromium 的开发者：4-7 天

**风险与常见坑：**

- **COMPONENT_EXPORT macro 名字错**：`services/network/public/cpp` 目录下的 export macro 大概率是 `NETWORK_CPP_BASE` 或 `NETWORK_CPP`。若编译报"undefined reference"，用 `grep -rn 'COMPONENT_EXPORT' services/network/public/cpp/*.h | head -5` 找邻居用什么
- **BUILD.gn 里的 target 分裂**：`network_cpp` 有多个 sub-target（比如 `cpp_base`, `cpp`, `cpp_client_certificate`），要加进能被 URLLoader 依赖的那个（通常是 `cpp_base` 或主 `cpp` 目标）；若 URLLoader 编译时找不到符号，检查 dep 链
- **URLLoader::StartRequest 分布多处**：Chromium 的 URLLoader 有 `URLLoader`、`SimpleURLLoader`、`ThrottlingURLLoader` 等多个实现；本 patch 只覆盖 `services/network/url_loader.cc` 里的核心 `URLLoader`，其他 loader 走的都是这个底层，理论上都覆盖到；若发现某些请求路径没走 gate，可能是 renderer 侧直接 fetch 走了别的路径（应该不会——网络服务化后所有请求都必经 URLLoader）
- **URLRequest API 版本差异**：`extra_request_headers()` 的返回类型 / `SetExtraRequestHeaders` 的签名在 Chromium 100+ 稳定，但更早版本可能不同；本文档假设 130 stable
- **Thread safety**：`GetByodRequestGate()` 在 network 线程调用，`SetByodRequestGate()` 在 browser main 线程调用；`GateHolder` 用 `base::Lock` 保护是必要的，不要用 `base::NoDestructor<std::unique_ptr>` 直接返回（无锁读写会 UB）
- **Test 5 的临时代码残留**：Task 5 结束后一定跑 `git status` 确认全清；如果不小心 commit 了临时 test helpers，用 `git reset HEAD~` 撤销
