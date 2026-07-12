# P1-E · 硬件指纹接口补丁 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 合入 spec §3.1 补丁 #15：定义 `HardwareAttestor` 抽象接口 + 三个具体实现（Windows TPM 2.0 存根、macOS Secure Enclave 存根、软指纹 fallback），供 P7 (Browser Exam Key 模块) 调用生成 `X-Exam-Client-Fingerprint`。

**Architecture:** 与 P1-C 网络接口和 P1-D IPC 接口一样，本 sub-plan 只做**接口冻结 + default 实现**：
- **抽象接口** `HardwareAttestor` 定义在 `chrome/browser/exam/attestation/`
- **软指纹 fallback**（无 TPM/无 SE 老机器用）—— 本 sub-plan 提供真实但**弱**实现（CPU serial + 主板 UUID + MAC 地址）
- **TPM / Secure Enclave 真实实现** —— 本 sub-plan 只提供**桩** (`kTpmNotImplementedYet` / `kSecureEnclaveNotImplementedYet`)，真实调用由 **P7** 交付（P7 有硬件密钥管理领域知识才写得对）

依赖 P1-A + P1-B + P1-C + P1-D 已合入（31 个 patch）。

**Tech Stack:** Chromium C++、平台桩（Win Tbs* API 桩、mac Security.framework 桩）、bash smoke tests。

## Global Constraints

- Chromium 补丁总量 ≤ 3000 行（全 P1 累计）；本批预算 ≤ 250 行；P1 累计目标 ≤ 2780 行
- Chromium 补丁只做"删/禁"和"钩子暴露"，**不写业务**；本 sub-plan 里的软指纹例外 —— 它是唯一的"完整实现"，因为没有硬件依赖，简单到不值得留给 P7
- 每个补丁必须独立可 revert；即使全部 revert 掉，Chromium 也能编译并启动
- 每个补丁的 commit message 前缀固定 `byod: `；spec 章节引用固定 `see spec §3.1 patch #N`
- 补丁文件命名遵循 P0 patch-workflow.md 的序号分段：`0900-0999` = Attestation
- P1-E 依赖 P1-A + P1-B + P1-C + P1-D 已合入（31 个 patch）
- **本 sub-plan 冻结的接口**：`HardwareAttestor::Quote` API —— P7 依赖，签名一经冻结不可改

---

## File Structure

**本 sub-plan 交付：**

```
/root/byod-browser/
├── patches/
│   ├── 0900-add-hardware-attestor-interface.patch    # 补丁 #15a (接口)
│   ├── 0901-add-software-fingerprint-attestor.patch  # 补丁 #15b (软指纹实现)
│   └── 0902-add-tpm-and-se-attestor-stubs.patch      # 补丁 #15c (TPM/SE 桩)
├── build/
│   └── smoke-test-p1e.sh
└── docs/
    └── development/
        └── p1-e-acceptance.md
```

**Chromium 源码触及区域：**

- 新增 `chrome/browser/exam/attestation/hardware_attestor.h` / `.cc` (#15a)
- 新增 `chrome/browser/exam/attestation/software_fingerprint_attestor.h` / `.cc` (#15b)
- 新增 `chrome/browser/exam/attestation/tpm_attestor_stub.cc` (#15c, Win only)
- 新增 `chrome/browser/exam/attestation/secure_enclave_attestor_stub.mm` (#15c, mac only)
- 修改 `chrome/browser/exam/BUILD.gn`

---

## Task 1: 补丁 #15a — HardwareAttestor 接口

**Files:**
- Create (in Chromium src): `chrome/browser/exam/attestation/hardware_attestor.h`
- Create (in Chromium src): `chrome/browser/exam/attestation/hardware_attestor.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0900-add-hardware-attestor-interface.patch`
- Create: `/root/byod-browser/build/smoke-test-p1e.sh`

**Interfaces:**
- Consumes: P1-D 全部完成（31 个 patch 已合入）
- **Produces**（P7 消费）:
  ```cpp
  namespace chrome::exam {

  enum class AttestationLevel {
    kSoft,           // Software-composed fingerprint (CPU + board + MAC).
    kTpm,            // Windows TPM 2.0.
    kSecureEnclave,  // macOS Secure Enclave.
  };

  struct AttestationQuote {
    AttestationLevel level;
    std::string fingerprint;  // Hex-encoded stable device ID.
    std::string signature;    // Empty for kSoft; hex-encoded EC/RSA
                              // signature for kTpm/kSecureEnclave.
    std::string public_key;   // Empty for kSoft; PEM-encoded for kTpm/kSE.
  };

  class HardwareAttestor {
   public:
    // Compute an attestation quote binding |nonce| to the device.
    // May be called from any thread; must be non-blocking (<50ms typical).
    virtual AttestationQuote Quote(std::string_view nonce) const = 0;

    // Reports which underlying capability this attestor uses.
    virtual AttestationLevel level() const = 0;

    virtual ~HardwareAttestor() = default;
  };

  void SetHardwareAttestor(std::unique_ptr<HardwareAttestor> attestor);
  HardwareAttestor* GetHardwareAttestor();

  }  // namespace chrome::exam
  ```

- [ ] **Step 1: 起始状态自检**

Run:
```bash
cd /opt/chromium/src
git status
git describe --tags
git log --oneline | head -32
```
Expected: `working tree clean`；tag 一致；最近 31 条 commit 是 P1-A + P1-B + P1-C + P1-D 的补丁。

若不干净：`echo y | bash /root/byod-browser/build/revert-patches.sh && bash /root/byod-browser/build/apply-patches.sh`

- [ ] **Step 2: 写 smoke-test-p1e.sh 骨架**

Write `/root/byod-browser/build/smoke-test-p1e.sh`:

```bash
#!/usr/bin/env bash
# Behavior smoke tests for P1-E patches (#15).
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

# ---------- Patch #15a: attestor interface header ----------
echo "[test] patch #15a: HardwareAttestor interface"
if [ -f /opt/chromium/src/chrome/browser/exam/attestation/hardware_attestor.h ]; then
  pass "#15a: hardware_attestor.h present"
else
  fail "#15a: hardware_attestor.h missing"
fi

# Verify chrome still boots
out="$(run_chrome --headless --dump-dom https://example.com 2>&1 || true)"
if echo "$out" | grep -q "Example Domain"; then
  pass "#15a: chrome boots after attestation wiring"
else
  fail "#15a: chrome broken after attestation wiring"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "[fail] one or more P1-E tests failed"; exit 1
fi
echo "[success] all P1-E tests passed"
```

Run:
```bash
chmod +x /root/byod-browser/build/smoke-test-p1e.sh
```

- [ ] **Step 3: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1e.sh`
Expected: `✗ #15a: hardware_attestor.h missing`

- [ ] **Step 4: 创建 hardware_attestor.h**

Write `/opt/chromium/src/chrome/browser/exam/attestation/hardware_attestor.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #15a (spec §3.1, §5.2.3): abstract hardware attestor. Real
// TPM/Secure Enclave implementations arrive in P7; P1-E ships this
// interface plus a software-fallback implementation (#15b) plus platform
// stubs (#15c) so P7 can slot in real code without touching #15a.

#ifndef CHROME_BROWSER_EXAM_ATTESTATION_HARDWARE_ATTESTOR_H_
#define CHROME_BROWSER_EXAM_ATTESTATION_HARDWARE_ATTESTOR_H_

#include <memory>
#include <string>
#include <string_view>

namespace chrome::exam {

enum class AttestationLevel {
  kSoft,           // Software fingerprint (CPU + board + MAC).
  kTpm,            // Windows TPM 2.0 (P7).
  kSecureEnclave,  // macOS Secure Enclave (P7).
};

struct AttestationQuote {
  AttestationLevel level = AttestationLevel::kSoft;
  std::string fingerprint;  // Hex-encoded stable device ID.
  std::string signature;    // Hex-encoded signature (empty for kSoft).
  std::string public_key;   // PEM public key (empty for kSoft).
};

class HardwareAttestor {
 public:
  // Compute a quote binding |nonce| to the device. Non-blocking (<50ms).
  // Thread-safe; called from network service thread by P7 injector.
  virtual AttestationQuote Quote(std::string_view nonce) const = 0;

  virtual AttestationLevel level() const = 0;

  virtual ~HardwareAttestor() = default;
};

// Install an attestor. Default is null (Quote() returns empty via
// GetHardwareAttestor() nullptr check in P7).
void SetHardwareAttestor(std::unique_ptr<HardwareAttestor> attestor);
HardwareAttestor* GetHardwareAttestor();

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_ATTESTATION_HARDWARE_ATTESTOR_H_
```

- [ ] **Step 5: 创建 hardware_attestor.cc**

Write `/opt/chromium/src/chrome/browser/exam/attestation/hardware_attestor.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/attestation/hardware_attestor.h"

#include "base/no_destructor.h"

namespace chrome::exam {

namespace {

std::unique_ptr<HardwareAttestor>& MutableAttestor() {
  static base::NoDestructor<std::unique_ptr<HardwareAttestor>> attestor;
  return *attestor;
}

}  // namespace

void SetHardwareAttestor(std::unique_ptr<HardwareAttestor> attestor) {
  MutableAttestor() = std::move(attestor);
}

HardwareAttestor* GetHardwareAttestor() {
  return MutableAttestor().get();
}

}  // namespace chrome::exam
```

- [ ] **Step 6: 修改 BUILD.gn**

在 `/opt/chromium/src/chrome/browser/exam/BUILD.gn` 的 `sources = [` 列表加入：

```gn
    "attestation/hardware_attestor.cc",
    "attestation/hardware_attestor.h",
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
bash /root/byod-browser/build/smoke-test-p1e.sh
```

- [ ] **Step 8: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/attestation/hardware_attestor.h \
        chrome/browser/exam/attestation/hardware_attestor.cc
git commit -m "byod: add HardwareAttestor interface (patch #15a)

Patch #15a (spec §3.1, §5.2.3). Abstract attestor with AttestationLevel /
AttestationQuote types. Default null; P7 installs concrete implementations."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0900-add-hardware-attestor-interface.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1e.sh

cd /root/byod-browser
git add patches/0900-add-hardware-attestor-interface.patch build/smoke-test-p1e.sh
git commit -m "patch: 0900 HardwareAttestor interface (patch #15a)

Est. LOC: ~90."
```

---

## Task 2: 补丁 #15b — 软指纹实现（fallback，跨平台）

**Files:**
- Create (in Chromium src): `chrome/browser/exam/attestation/software_fingerprint_attestor.h`
- Create (in Chromium src): `chrome/browser/exam/attestation/software_fingerprint_attestor.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0901-add-software-fingerprint-attestor.patch`

**Interfaces:**
- Consumes: Task 1 完成
- **Produces**（P7 消费）:
  ```cpp
  namespace chrome::exam {

  // Cross-platform fallback: combines CPU model + board UUID (Win: SMBIOS,
  // mac: IOPlatformUUID) + primary NIC MAC into a stable SHA-256 hex string.
  // No hardware key; signature and public_key are empty in the quote.
  class SoftwareFingerprintAttestor : public HardwareAttestor {
   public:
    AttestationQuote Quote(std::string_view nonce) const override;
    AttestationLevel level() const override {
      return AttestationLevel::kSoft;
    }
  };

  }  // namespace chrome::exam
  ```

**说明**：这是 P1-E 唯一"完整实现"的 attestor。TPM 和 Secure Enclave 需要硬件领域知识（TPM PCR、EK cert 链、SE key gen 与 sign 的 iOS/Mac 差异），P7 时才写。软指纹只是 hash 一些可读的系统标识，写在 P1-E 里 OK。

**弱点**（诚实标注）：软指纹**不能防学生换机**（换主板 MAC 会变）；也**不能提供不可伪造性**（学生 root 后可以伪造 CPU serial / MAC）。真正的 attestation 一定要走硬件密钥。软指纹的价值是"比什么都没有强"—— 服务端至少能识别"同一台机器多场次"。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1e.sh`:

```bash
# ---------- Patch #15b: software fingerprint attestor ----------
echo "[test] patch #15b: SoftwareFingerprintAttestor"
if [ -f /opt/chromium/src/chrome/browser/exam/attestation/software_fingerprint_attestor.h ]; then
  pass "#15b: software_fingerprint_attestor.h present"
else
  fail "#15b: software_fingerprint_attestor.h missing"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1e.sh`

- [ ] **Step 3: 创建 software_fingerprint_attestor.h**

Write `/opt/chromium/src/chrome/browser/exam/attestation/software_fingerprint_attestor.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #15b (spec §5.2.3 downgrade path): software fingerprint
// attestor — used when TPM/Secure Enclave is unavailable. Weak: cannot
// prove non-forgery, only stability across sessions on the same machine.

#ifndef CHROME_BROWSER_EXAM_ATTESTATION_SOFTWARE_FINGERPRINT_ATTESTOR_H_
#define CHROME_BROWSER_EXAM_ATTESTATION_SOFTWARE_FINGERPRINT_ATTESTOR_H_

#include "chrome/browser/exam/attestation/hardware_attestor.h"

namespace chrome::exam {

class SoftwareFingerprintAttestor : public HardwareAttestor {
 public:
  AttestationQuote Quote(std::string_view nonce) const override;
  AttestationLevel level() const override {
    return AttestationLevel::kSoft;
  }
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_ATTESTATION_SOFTWARE_FINGERPRINT_ATTESTOR_H_
```

- [ ] **Step 4: 创建 software_fingerprint_attestor.cc**

Write `/opt/chromium/src/chrome/browser/exam/attestation/software_fingerprint_attestor.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/attestation/software_fingerprint_attestor.h"

#include "base/hash/sha1.h"
#include "base/strings/string_number_conversions.h"
#include "base/system/sys_info.h"
#include "crypto/sha2.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#elif BUILDFLAG(IS_MAC)
#include <sys/sysctl.h>
#endif

namespace chrome::exam {

namespace {

// Combine several stable system strings into a single blob. Missing pieces
// are represented as literal "unknown". Order matters (must be stable for
// hash reproducibility across process restarts on the same box).
std::string CollectSystemIdentifiers() {
  std::string cpu_brand = base::SysInfo::CPUModelName();
  if (cpu_brand.empty()) cpu_brand = "unknown-cpu";

  std::string os_arch = base::SysInfo::OperatingSystemArchitecture();
  if (os_arch.empty()) os_arch = "unknown-arch";

  // Platform-specific board identifier.
  std::string board_id = "unknown-board";
#if BUILDFLAG(IS_MAC)
  size_t size = 0;
  if (sysctlbyname("hw.model", nullptr, &size, nullptr, 0) == 0 && size > 0) {
    std::string tmp(size, '\0');
    if (sysctlbyname("hw.model", tmp.data(), &size, nullptr, 0) == 0) {
      // Trim trailing NUL.
      while (!tmp.empty() && tmp.back() == '\0') tmp.pop_back();
      board_id = tmp;
    }
  }
#elif BUILDFLAG(IS_WIN)
  // ComputerName is not a board id, but is stable enough for the soft-level
  // fingerprint. Real Windows path in P7 uses SMBIOS via GetSystemFirmwareTable.
  wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD sz = std::size(name);
  if (GetComputerNameW(name, &sz)) {
    board_id = base::WideToUTF8(std::wstring(name, sz));
  }
#elif BUILDFLAG(IS_LINUX)
  // Linux dev build: use hostname as a placeholder.
  char host[256] = {};
  if (gethostname(host, sizeof(host) - 1) == 0) board_id = host;
#endif

  return cpu_brand + "|" + os_arch + "|" + board_id;
}

}  // namespace

AttestationQuote SoftwareFingerprintAttestor::Quote(
    std::string_view nonce) const {
  const std::string ids = CollectSystemIdentifiers();
  const std::string combined = ids + "|nonce=" + std::string(nonce);

  const std::string sha = crypto::SHA256HashString(combined);

  AttestationQuote quote;
  quote.level = AttestationLevel::kSoft;
  quote.fingerprint = base::HexEncode(sha.data(), sha.size());
  // No signature / public key for soft level per interface contract.
  return quote;
}

}  // namespace chrome::exam
```

**注意**：
- `base::WideToUTF8` 需要 `#include "base/strings/utf_string_conversions.h"`
- `gethostname` 在 Linux 需 `#include <unistd.h>`
- 若 Chromium 版本没有 `crypto::SHA256HashString`，用 `crypto::hash::Sha256(...)` 或 `base::SHA1HashString`（Sha1 弱一点但仍稳定）
- 平台宏 `BUILDFLAG(IS_LINUX)` 若不识别，改用 `defined(__linux__)`

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "attestation/software_fingerprint_attestor.cc",
    "attestation/software_fingerprint_attestor.h",
```

deps 若无 crypto 需加：

```gn
    "//crypto",
```

- [ ] **Step 6: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**若报"CPUModelName 未定义"**：某些 Chromium 版本里 `SysInfo::CPUModelName()` 只在 x86 上定义，其它平台走 `HardwareModelName()`。改用后者更通用。

- [ ] **Step 7: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
bash /root/byod-browser/build/smoke-test-p1e.sh
```

- [ ] **Step 8: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/attestation/software_fingerprint_attestor.h \
        chrome/browser/exam/attestation/software_fingerprint_attestor.cc
git commit -m "byod: add SoftwareFingerprintAttestor (patch #15b)

Patch #15b (spec §5.2.3 downgrade path). Cross-platform SHA-256 of
CPU model + arch + board id + nonce. No hardware key; use only when
TPM/Secure Enclave unavailable."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0901-add-software-fingerprint-attestor.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1e.sh

cd /root/byod-browser
git add patches/0901-add-software-fingerprint-attestor.patch build/smoke-test-p1e.sh
git commit -m "patch: 0901 software fingerprint attestor (patch #15b)

Est. LOC: ~90."
```

---

## Task 3: 补丁 #15c — TPM & Secure Enclave 存根（Win/mac 平台桩）

**Files:**
- Create (in Chromium src): `chrome/browser/exam/attestation/tpm_attestor_stub.h`
- Create (in Chromium src): `chrome/browser/exam/attestation/tpm_attestor_stub.cc`
- Create (in Chromium src): `chrome/browser/exam/attestation/secure_enclave_attestor_stub.h`
- Create (in Chromium src): `chrome/browser/exam/attestation/secure_enclave_attestor_stub.mm`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0902-add-tpm-and-se-attestor-stubs.patch`

**Interfaces:**
- Consumes: Task 2 完成
- **Produces**（P7 覆写这两个 stub 为真实实现）:
  ```cpp
  namespace chrome::exam {

  // Windows TPM 2.0 attestor stub. P7 implements real Tbs* API calls +
  // EK/AK cert chain.
  class TpmAttestor : public HardwareAttestor {
   public:
    // Returns false if TPM not present or inaccessible.
    static bool IsAvailable();

    AttestationQuote Quote(std::string_view nonce) const override;
    AttestationLevel level() const override {
      return AttestationLevel::kTpm;
    }
  };

  // macOS Secure Enclave attestor stub. P7 implements real
  // SecKeyCreateRandomKey with kSecAttrTokenIDSecureEnclave.
  class SecureEnclaveAttestor : public HardwareAttestor {
   public:
    static bool IsAvailable();
    AttestationQuote Quote(std::string_view nonce) const override;
    AttestationLevel level() const override {
      return AttestationLevel::kSecureEnclave;
    }
  };

  }  // namespace chrome::exam
  ```

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1e.sh`:

```bash
# ---------- Patch #15c: TPM/SE attestor stubs ----------
echo "[test] patch #15c: TPM and Secure Enclave stubs"
if [ -f /opt/chromium/src/chrome/browser/exam/attestation/tpm_attestor_stub.h ]; then
  pass "#15c-tpm: tpm_attestor_stub.h present"
else
  fail "#15c-tpm: tpm_attestor_stub.h missing"
fi
if [ -f /opt/chromium/src/chrome/browser/exam/attestation/secure_enclave_attestor_stub.h ]; then
  pass "#15c-se: secure_enclave_attestor_stub.h present"
else
  fail "#15c-se: secure_enclave_attestor_stub.h missing"
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1e.sh`

- [ ] **Step 3: 创建 tpm_attestor_stub.h**

Write `/opt/chromium/src/chrome/browser/exam/attestation/tpm_attestor_stub.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #15c (spec §5.2.3, P7 handoff): Windows TPM 2.0 attestor stub.
// P1-E ships a stub whose Quote() logs and returns an empty signature.
// P7 replaces the .cc implementation with real Tbs*/NCrypt* calls.

#ifndef CHROME_BROWSER_EXAM_ATTESTATION_TPM_ATTESTOR_STUB_H_
#define CHROME_BROWSER_EXAM_ATTESTATION_TPM_ATTESTOR_STUB_H_

#include "chrome/browser/exam/attestation/hardware_attestor.h"

namespace chrome::exam {

class TpmAttestor : public HardwareAttestor {
 public:
  // Returns true only on Windows with TPM 2.0. Stub: always returns false.
  static bool IsAvailable();

  AttestationQuote Quote(std::string_view nonce) const override;
  AttestationLevel level() const override {
    return AttestationLevel::kTpm;
  }
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_ATTESTATION_TPM_ATTESTOR_STUB_H_
```

- [ ] **Step 4: 创建 tpm_attestor_stub.cc**

Write `/opt/chromium/src/chrome/browser/exam/attestation/tpm_attestor_stub.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/attestation/tpm_attestor_stub.h"

#include "base/logging.h"

namespace chrome::exam {

// static
bool TpmAttestor::IsAvailable() {
  // P1-E stub: unconditional false so soft fallback is always chosen.
  // P7 replaces with: check Tbsi_Context_Create returns TBS_SUCCESS.
  return false;
}

AttestationQuote TpmAttestor::Quote(std::string_view nonce) const {
  LOG(WARNING) << "[byod] TpmAttestor::Quote called on stub — no signature";
  AttestationQuote quote;
  quote.level = AttestationLevel::kTpm;
  quote.fingerprint = "TPM_STUB_NOT_IMPLEMENTED_YET";
  return quote;
}

}  // namespace chrome::exam
```

- [ ] **Step 5: 创建 secure_enclave_attestor_stub.h**

Write `/opt/chromium/src/chrome/browser/exam/attestation/secure_enclave_attestor_stub.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD patch #15c (spec §5.2.3, P7 handoff): macOS Secure Enclave attestor
// stub. P7 replaces with SecKeyCreateRandomKey + SecKeyCreateSignature.

#ifndef CHROME_BROWSER_EXAM_ATTESTATION_SECURE_ENCLAVE_ATTESTOR_STUB_H_
#define CHROME_BROWSER_EXAM_ATTESTATION_SECURE_ENCLAVE_ATTESTOR_STUB_H_

#include "chrome/browser/exam/attestation/hardware_attestor.h"

namespace chrome::exam {

class SecureEnclaveAttestor : public HardwareAttestor {
 public:
  // Returns true only on Apple Silicon Macs with Secure Enclave. Stub: false.
  static bool IsAvailable();

  AttestationQuote Quote(std::string_view nonce) const override;
  AttestationLevel level() const override {
    return AttestationLevel::kSecureEnclave;
  }
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_ATTESTATION_SECURE_ENCLAVE_ATTESTOR_STUB_H_
```

- [ ] **Step 6: 创建 secure_enclave_attestor_stub.mm**

Write `/opt/chromium/src/chrome/browser/exam/attestation/secure_enclave_attestor_stub.mm`:

```objc
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/attestation/secure_enclave_attestor_stub.h"

#include "base/logging.h"

namespace chrome::exam {

// static
bool SecureEnclaveAttestor::IsAvailable() {
  // P1-E stub: false. P7 replaces with runtime check via SecKeyCreateRandomKey
  // attempt with kSecAttrTokenIDSecureEnclave.
  return false;
}

AttestationQuote SecureEnclaveAttestor::Quote(std::string_view nonce) const {
  LOG(WARNING)
      << "[byod] SecureEnclaveAttestor::Quote called on stub — no signature";
  AttestationQuote quote;
  quote.level = AttestationLevel::kSecureEnclave;
  quote.fingerprint = "SE_STUB_NOT_IMPLEMENTED_YET";
  return quote;
}

}  // namespace chrome::exam
```

- [ ] **Step 7: 修改 BUILD.gn — 平台特化 sources**

在 `/opt/chromium/src/chrome/browser/exam/BUILD.gn` 的 `sources = [` 列表加**跨平台**部分：

```gn
    "attestation/tpm_attestor_stub.h",
    "attestation/secure_enclave_attestor_stub.h",
```

然后加**平台条件**编译（在 `source_set("exam")` target 内、`sources` 之后）：

```gn
  if (is_win) {
    sources += [ "attestation/tpm_attestor_stub.cc" ]
  }
  if (is_mac) {
    sources += [ "attestation/secure_enclave_attestor_stub.mm" ]
  }
```

**说明**：TPM 桩 `.cc` 不含平台特化 API（真实的 Tbs* API 只有 P7 加），所以理论上可以跨平台编译；但语义上归 Windows，用条件编译隔离更清晰。同理 mac 的 `.mm`。

Linux dev build 时**两个 .cc/.mm 都不参与编译** —— header 存在（可以被 include 到），但符号不解析。这对 P1-E 里 Linux smoke 无影响，因为**本 sub-plan 里 chromium 不调用 TpmAttestor / SecureEnclaveAttestor**（P7 才会 install）。

- [ ] **Step 8: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**若 Linux 上因未定义符号报错**：说明有代码路径已引用了 TpmAttestor / SecureEnclaveAttestor。P1-E 里不该有；如果有，检查是否哪个 forward declaration 引发的问题；改成把 header include 但 impl 不 include。

- [ ] **Step 9: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
bash /root/byod-browser/build/smoke-test-p1e.sh
```

- [ ] **Step 10: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/attestation/tpm_attestor_stub.h \
        chrome/browser/exam/attestation/tpm_attestor_stub.cc \
        chrome/browser/exam/attestation/secure_enclave_attestor_stub.h \
        chrome/browser/exam/attestation/secure_enclave_attestor_stub.mm
git commit -m "byod: add TPM and Secure Enclave attestor stubs (patch #15c)

Patch #15c (spec §5.2.3, P7 handoff). Platform-specific stubs:
IsAvailable() returns false so soft fallback is chosen; Quote() returns
'*_STUB_NOT_IMPLEMENTED_YET'. Real Tbs*/NCrypt* and SecKey* code is
P7's responsibility."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0902-add-tpm-and-se-attestor-stubs.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1e.sh

cd /root/byod-browser
git add patches/0902-add-tpm-and-se-attestor-stubs.patch build/smoke-test-p1e.sh
git commit -m "patch: 0902 TPM & Secure Enclave stubs (patch #15c)

Est. LOC: ~70."
```

---

## Task 4: 端到端集成测试 — 手工验证软指纹的稳定性

**Files:**
- Create (in Chromium src, temporary): `chrome/browser/exam/attestation/test_install.cc`
- Modify (in Chromium src, temporary): `chrome/browser/chrome_browser_main.cc` — 装 SoftwareFingerprintAttestor
- **不导出 patch**

**Interfaces:**
- Consumes: Task 3 完成
- Produces: 一次性行为验证 —— 证明 SoftwareFingerprintAttestor::Quote 能被调用、多次调用同 nonce 的 fingerprint 相同、不同 nonce 的 fingerprint 不同

- [ ] **Step 1: 写测试安装 + 打印 fingerprint**

Write `/opt/chromium/src/chrome/browser/exam/attestation/test_install.cc`（临时）:

```cpp
// TEMPORARY: BYOD P1-E Task 4 verification. Delete before commit.
#include "base/logging.h"
#include "chrome/browser/exam/attestation/hardware_attestor.h"
#include "chrome/browser/exam/attestation/software_fingerprint_attestor.h"

namespace chrome::exam {

void InstallSoftFingerprintForTest() {
  SetHardwareAttestor(std::make_unique<SoftwareFingerprintAttestor>());
  auto* attestor = GetHardwareAttestor();
  if (!attestor) {
    LOG(ERROR) << "[byod-test] attestor not installed";
    return;
  }
  auto q1 = attestor->Quote("nonce-A");
  auto q2 = attestor->Quote("nonce-A");
  auto q3 = attestor->Quote("nonce-B");
  LOG(INFO) << "[byod-test] Q(nonce-A)#1 = " << q1.fingerprint;
  LOG(INFO) << "[byod-test] Q(nonce-A)#2 = " << q2.fingerprint;
  LOG(INFO) << "[byod-test] Q(nonce-B)   = " << q3.fingerprint;
  LOG(INFO) << "[byod-test] same_A_A = " << (q1.fingerprint == q2.fingerprint);
  LOG(INFO) << "[byod-test] diff_A_B = " << (q1.fingerprint != q3.fingerprint);
}

}  // namespace chrome::exam
```

- [ ] **Step 2: 临时挂到 browser main**

在 `chrome_browser_main.cc` 的 `PreCreateThreads` 尾部添加：

```cpp
  // TEMP P1-E Task 4 — DELETE after test.
  extern void InstallSoftFingerprintForTest();
  chrome::exam::InstallSoftFingerprintForTest();
```

BUILD.gn 临时加 `"attestation/test_install.cc"`。

- [ ] **Step 3: 编译 + 启动 + 观察 log**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome

/opt/chromium/src/out/linux/chrome \
  --user-data-dir=/tmp/byod-p1e-t4 \
  --no-first-run --no-sandbox --disable-gpu \
  --enable-logging=stderr --v=0 \
  --headless --dump-dom https://example.com 2>&1 | grep -E "\[byod-test\]"
```
Expected:
```
[byod-test] Q(nonce-A)#1 = <64-hex-chars>
[byod-test] Q(nonce-A)#2 = <same 64-hex-chars>
[byod-test] Q(nonce-B)   = <different 64-hex-chars>
[byod-test] same_A_A = 1
[byod-test] diff_A_B = 1
```

- [ ] **Step 4: 移除测试代码**

Run:
```bash
cd /opt/chromium/src
rm chrome/browser/exam/attestation/test_install.cc
git checkout chrome/browser/chrome_browser_main.cc
git checkout chrome/browser/exam/BUILD.gn
git status
```
Expected: 干净

- [ ] **Step 5: 再编 + 全量 smoke 验证残留清除**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
bash /root/byod-browser/build/smoke-test-p1e.sh
```

本 task 不 commit。

---

## Task 5: 补丁总量预算校验 + P1 整批交付确认

**Files:**
- Create: `/root/byod-browser/docs/development/p1-e-acceptance.md`
- Create: `/root/byod-browser/docs/development/p1-full-acceptance.md`

**Interfaces:**
- Consumes: Task 1..4 全部完成
- Produces: P1-E 单批验收 + **P1 全局验收清单**

- [ ] **Step 1: 统计当前 patch 总行数**

Run:
```bash
source /root/byod-browser/build/chromium-version.env
cd /opt/chromium/src
git diff --shortstat "$CHROMIUM_TAG"..HEAD
```
Expected: P1 全部 insertions ≤ 2780（P1-A 600 + P1-B 580 + P1-C 450 + P1-D 900 + P1-E 250）；**最终 ≤ spec §3.1 的 3000 行硬预算**

- [ ] **Step 2: 数累计补丁文件数**

Run:
```bash
ls /root/byod-browser/patches/*.patch | wc -l
```
Expected: **34**（P1-A 9 + P1-B 9 + P1-C 4 + P1-D 9 + P1-E 3）

- [ ] **Step 3: 全量 revert + apply + 编译 + 全量 smoke**

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
bash /root/byod-browser/build/smoke-test-p1e.sh
```
Expected: 全部成功

- [ ] **Step 4: 写 p1-e-acceptance.md**

Write `/root/byod-browser/docs/development/p1-e-acceptance.md`:

```markdown
# P1-E 交付验收 Checklist

## 补丁类

- [ ] `patches/0900-add-hardware-attestor-interface.patch` 存在
- [ ] `patches/0901-add-software-fingerprint-attestor.patch` 存在
- [ ] `patches/0902-add-tpm-and-se-attestor-stubs.patch` 存在

## 编译类

- [ ] `bash build/revert-patches.sh && bash build/apply-patches.sh` 无冲突
- [ ] `bash build/build-linux.sh chrome` 成功
- [ ] `git diff --shortstat "$CHROMIUM_TAG"..HEAD` insertions ≤ 2780（P1 全部累计）

## 行为类（Linux headless smoke tests）

- [ ] `bash build/smoke-test.sh` 全部 pass
- [ ] `bash build/smoke-test-p1a.sh` 全部 pass
- [ ] `bash build/smoke-test-p1b.sh` 全部 pass
- [ ] `bash build/smoke-test-p1c.sh` 全部 pass
- [ ] `bash build/smoke-test-p1d.sh` 全部 pass
- [ ] `bash build/smoke-test-p1e.sh` 全部 pass

## 端到端集成类（Task 4）

- [ ] 装 SoftwareFingerprintAttestor 后，log 输出 `[byod-test]` 三行 fingerprint
- [ ] Q(nonce-A) 两次结果相同（软指纹稳定性）
- [ ] Q(nonce-A) 与 Q(nonce-B) 不同（nonce 生效）
- [ ] Task 4 结束后测试代码全部 revert，`git status` 干净

## 接口冻结类（供 P7 消费）

- [ ] `chrome::exam::HardwareAttestor` / `AttestationLevel` / `AttestationQuote` 已 export
- [ ] `chrome::exam::SoftwareFingerprintAttestor` 已 export（P7 用作 fallback）
- [ ] `chrome::exam::TpmAttestor` stub 已 export（P7 override .cc）
- [ ] `chrome::exam::SecureEnclaveAttestor` stub 已 export（P7 override .mm）

## 文档类

- [ ] 本文件（p1-e-acceptance.md）存在
- [ ] `docs/development/p1-full-acceptance.md` 存在（本 Task Step 5）
- [ ] roadmap 里 P1 状态更新为 `✅ 完成`

## 交付判定

以上全部打勾 → P1-E 完成，P1 完全交付，可进入 P2..P11。
```

- [ ] **Step 5: 写 P1 全局验收 p1-full-acceptance.md**

Write `/root/byod-browser/docs/development/p1-full-acceptance.md`:

```markdown
# P1 全局验收清单（P1-A + P1-B + P1-C + P1-D + P1-E）

## 补丁总量

- [ ] `ls /root/byod-browser/patches/*.patch | wc -l` = 34
- [ ] `git diff --shortstat "$CHROMIUM_TAG"..HEAD` insertions 净增 ≤ 3000（spec §3.1 硬预算）
- [ ] `git log --oneline "$CHROMIUM_TAG"..HEAD | wc -l` = 34（一 patch 一 commit）
- [ ] 所有 commit message 前缀均为 `byod:`

## 5 批分别验收

- [ ] P1-A acceptance 全部打勾 → `docs/development/p1-a-acceptance.md`
- [ ] P1-B acceptance 全部打勾 → `docs/development/p1-b-acceptance.md`
- [ ] P1-C acceptance 全部打勾 → `docs/development/p1-c-acceptance.md`
- [ ] P1-D acceptance 全部打勾 → `docs/development/p1-d-acceptance.md`
- [ ] P1-E acceptance 全部打勾 → `docs/development/p1-e-acceptance.md`

## 端到端编译验证

- [ ] 从干净 vanilla Chromium `bash build/revert-patches.sh` → `bash build/apply-patches.sh` 应用全部 34 patch 无冲突
- [ ] `bash build/build-linux.sh chrome` 一次编译成功
- [ ] 6 个 smoke test（`smoke-test.sh` + `smoke-test-p1a.sh` + `smoke-test-p1b.sh` + `smoke-test-p1c.sh` + `smoke-test-p1d.sh` + `smoke-test-p1e.sh`）全 pass

## 跨平台编译验证（若资源到位）

- [ ] `bash build/build-macos.sh chrome` 在 mac 上成功
- [ ] `powershell -File build\build-windows.ps1` 在 Win 上成功

## 接口冻结清单（供 P2..P11）

**Network 层（P1-C）**：
- [ ] `network::ByodRequestGate` + `SetByodRequestGate` + `GetByodRequestGate`
- [ ] `network::ByodHeaderInjector` + `SetByodHeaderInjector` + `GetByodHeaderInjector`
- [ ] `network::ByodExamHeaders`（4 字段：auth / timestamp / nonce / fingerprint）

**IPC 层（P1-D）**：
- [ ] `chrome::exam::GuardianTransport` + `SetGuardianTransport` + `GetGuardianTransport`
- [ ] `chrome::exam::GuardianFrame` + `MessageType` 枚举
- [ ] `chrome::exam::HeartbeatStateMachine` + 5 个常量（kInterval, kDegraded, kCritical, kBurn, kWallClockLimit）
- [ ] `chrome::exam::TryLaunchGuardian` + `GetGuardianExecutablePath`

**Session 层（P1-D）**：
- [ ] `chrome::exam::ExamSession::Get()` 单例
- [ ] `chrome::exam::SessionState` 枚举（kIdle/kActive/kBurning/kEnded）
- [ ] `ExamSession::StartExam` / `EndExam` / `TriggerBurn` / `AddObserver`

**Events 层（P1-D）**：
- [ ] `chrome::exam::ExamEventSink` + `SetExamEventSink` + `GetExamEventSink` + `EmitEvent`
- [ ] `chrome::exam::ExamEvent` + `ExamEventType`
- [ ] BrowserView 自动 emit blur/focus/resize；WebContents 自动 emit navigation

**Attestation 层（P1-E）**：
- [ ] `chrome::exam::HardwareAttestor` + `AttestationLevel` + `AttestationQuote`
- [ ] `chrome::exam::SoftwareFingerprintAttestor`（可直接用）
- [ ] `chrome::exam::TpmAttestor` / `SecureEnclaveAttestor` 桩（P7 替换）

## 通过判定

以上全部打勾 → P1 完成，可以启动 P2..P11。
```

- [ ] **Step 6: Commit 两个 acceptance + 更新 roadmap**

Run:
```bash
cd /root/byod-browser
git add docs/development/p1-e-acceptance.md docs/development/p1-full-acceptance.md
git commit -m "docs: add P1-E and P1-full acceptance checklists"
```

编辑 `/root/byod-browser/docs/superpowers/plans/2026-07-11-byod-roadmap.md`：
- 把 P1 那一行状态从 `⏳ 进行中 (...)` 改成 `✅ 完成`

```bash
cd /root/byod-browser
git add docs/superpowers/plans/2026-07-11-byod-roadmap.md
git commit -m "docs: mark P1 complete in roadmap"
```

---

## P1-E 完成后

**P1 完全交付**：
- 3 个新补丁（本批）+ 31 个先前补丁 = **34 个补丁**
- 累计 patch 行数 ≤ 2780 / 3000 硬预算（**留 220 行 margin 给 rebase 时冲突调整**）
- **17 个接口冻结**，P2..P11 全部 sub-plan 都有可对接的钩子
- 6 个 smoke test 脚本可跑

**下一步**：
- 若还没启动实施：开始 P0（Chromium 首编环境）
- 若 P0 已完成：可以启动 P1 五批分批实施（顺序 P1-A → P1-B → P1-C → P1-D → P1-E）
- 若 P1 也已完成：进入 P2..P11 的 sub-plan 写作

**预计 P1-E 实际耗时**（含每次增量编译）：
- 熟练 Chromium 开发者：1-2 天
- 首次改 Chromium 的开发者：3-5 天

**风险与常见坑：**

- **软指纹平台差异**：Linux 上没有可靠的 board id，本 sub-plan 用 hostname 代替；生产 build 不跑 Linux，这只是 dev 便利，不影响 Win/mac 行为
- **TPM/SE 桩的 IsAvailable 返回 false 是故意的**：P1-E 阶段保证 fallback 到软指纹；P7 里改成真实检测
- **base::NoDestructor<std::unique_ptr>` 单例线程安全**：本 sub-plan 里 `MutableAttestor` 未加锁 —— 遵循与 P1-C/P1-D 相同的假设（Set 在 startup 一次、Get 在多线程）；若 P7 里 Set 变成运行时可切换，P7 里加锁
- **crypto::SHA256HashString 在新版 Chromium 更名为 `crypto::hash::Sha256`**：按实际报错切换
- **`.mm` 在 Linux 不编译**：本 sub-plan 依赖此假设；若 SecureEnclaveAttestor 类被 Linux 代码路径引用会 link 失败 —— P1-E 里没引用，OK；P7 里可能会（P7 用 `#if BUILDFLAG(IS_MAC)` 保护）
