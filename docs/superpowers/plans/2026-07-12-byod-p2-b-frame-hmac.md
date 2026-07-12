# P2-B · 消息帧编解码 & HMAC Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 spec §5.1.3 定义的 Browser↔Guardian **消息帧编解码** + **HMAC-SHA256 完整性校验** —— 把 P1-D 里 `GuardianFrame::hmac` 的空字节段换成真实签名/校验，并给消息帧一个稳定的 wire format 让 Rust/Swift 端能对接。含**序号单调递增**防重放。

**Architecture:** 三个组件：
- **`FrameCodec`**：把 `GuardianFrame` (spec §5.1) 序列化为字节 buffer / 反序列化回结构体；wire format 用小端序 fixed-size header + 变长 payload，避免 protobuf 依赖（Guardian 侧 Rust 不想拉整个 prost 生态）
- **`FrameHmac`**：用 `IpcKey`（P2-A 冻结）算/校验 HMAC-SHA256；接受 `GuardianFrame` 返回 32 字节 tag，或校验 tag 匹配
- **`SequenceGuard`**：单调递增序号防重放，每方向独立计数 —— Browser→Guardian 一份、Guardian→Browser 一份；接收方拒绝任何 seq ≤ 已见 max

**本 sub-plan 不做**（P2-C/D/E 交付）：真实的 socket read/write loop、心跳 driver、challenge issuer、launcher 端到端集成。本 sub-plan 只做**纯函数式 codec + HMAC + 序号校验**，不涉及 IO。

**Tech Stack:** Chromium C++、`crypto::HMAC`、`base::span`、bash smoke tests + gtest 单元测试。

## Global Constraints

- Chromium 补丁总量 ≤ 3000 行硬预算；P1 已用 2780；本 sub-plan **全在新目录 `chrome/browser/exam/ipc/wire/`**，仅几行 BUILD.gn 修改属于"改现有 Chromium 代码"；预算内
- Chromium 补丁只做"删/禁"和"钩子暴露"；P2-B 是**实现层**，不新增 Chromium 补丁
- 补丁文件命名遵循 P0 patch-workflow.md 序号分段：`0600-0699` 是 IPC 与 Guardian；P2-A 已用 0610/0611/0612/0613/0614/0615/0616；P2-B 用 **0620/0621/0622/0623**（4 个 patch = codec + hmac + seq guard + gtest bundle）
- P2-B 依赖 P2-A 全部完成（41 个 patch 已合入）
- **Wire format 一经冻结不可改**（Rust/Swift Guardian 端会依赖），任何后续更改需**版本号 bump**（`GuardianFrame::type` 高 4 位保留给版本，本 sub-plan 用版本 1）

---

## File Structure

**本 sub-plan 交付：**

```
/root/byod-browser/
├── patches/
│   ├── 0620-add-frame-codec.patch                # FrameCodec 序列化/反序列化
│   ├── 0621-add-frame-hmac.patch                 # HMAC-SHA256 签名/校验
│   ├── 0622-add-sequence-guard.patch             # 单调序号防重放
│   └── 0623-add-frame-wire-unit-tests.patch      # gtest bundle
├── build/
│   └── smoke-test-p2b.sh
└── docs/
    └── development/
        ├── p2-b-acceptance.md
        └── ipc-wire-format.md                    # 冻结的 wire format 规范
```

**Chromium 源码触及区域：**

- 新增 `chrome/browser/exam/ipc/wire/frame_codec.h` + `.cc`
- 新增 `chrome/browser/exam/ipc/wire/frame_hmac.h` + `.cc`
- 新增 `chrome/browser/exam/ipc/wire/sequence_guard.h` + `.cc`
- 新增 `chrome/browser/exam/ipc/wire/frame_codec_unittest.cc`
- 新增 `chrome/browser/exam/ipc/wire/frame_hmac_unittest.cc`
- 新增 `chrome/browser/exam/ipc/wire/sequence_guard_unittest.cc`
- 修改 `chrome/browser/exam/BUILD.gn` — 加 sources
- 修改 `chrome/test/BUILD.gn` — 加 unittest sources

**接口冻结契约（P2-C/D/E + P3/P4 消费）：**

```cpp
namespace chrome::exam {

// FrameCodec: pure functions.
namespace frame_codec {

// Serialize frame to bytes. Returns nullopt if payload exceeds max size.
std::optional<std::vector<uint8_t>> Encode(const GuardianFrame& frame);

// Deserialize bytes to frame. Returns nullopt on any parse error
// (truncation, bad magic, unknown version).
std::optional<GuardianFrame> Decode(base::span<const uint8_t> bytes);

// Wire limits (per spec §5.1.3).
constexpr size_t kMaxPayloadBytes = 64 * 1024;   // 64 KiB
constexpr size_t kHeaderBytes = 24;              // Fixed header size
constexpr uint32_t kMagic = 0x42594F44;          // 'BYOD'

}  // namespace frame_codec

// FrameHmac: pure functions, takes IpcKey by reference (never copies).
namespace frame_hmac {

// Compute 32-byte HMAC-SHA256 over encoded frame excluding the hmac
// trailer field (i.e. over |header + payload|). Returns 32 bytes.
std::vector<uint8_t> Compute(const IpcKey& key,
                             base::span<const uint8_t> encoded_header_and_payload);

// Verify frame.hmac matches Compute(key, header+payload). Constant-time.
bool Verify(const IpcKey& key, const GuardianFrame& frame);

}  // namespace frame_hmac

// SequenceGuard: not thread-safe; per-direction instance.
class SequenceGuard {
 public:
  // Accept iff seq > highest previously accepted. Marks it as seen.
  bool AcceptAndAdvance(uint64_t seq);

  // Reserve next outbound seq (returns highest_+1 and advances).
  uint64_t NextOutbound();

  uint64_t highest_seen() const { return highest_; }

 private:
  uint64_t highest_ = 0;
};

}  // namespace chrome::exam
```

---

## Wire Format 冻结规范

**目的**：让本 sub-plan 结束时 Rust/Swift Guardian 端**已经能实施**，无需再谈判 wire format。

**帧结构**（网络字节序 = big-endian，除标记明确的 little-endian 部分外，本项目**全部使用 little-endian** 因为 x86 / ARM64 都是 LE）：

```
Offset  Size  Field                     Notes
------  ----  -----------------------   -------------------------------------
0       4     magic                     little-endian 0x42594F44 ('BYOD' LE)
                                        Guardian rejects frames without magic.
4       4     version_and_type          low 24 bits = MessageType (P1-D enum);
                                        high 8 bits = wire version (v1 = 1)
8       8     seq                       little-endian uint64
16      4     payload_len               little-endian uint32; ≤ 65536
20      4     reserved                  MUST be 0; deserializer rejects nonzero
--- header end (24 bytes) ---
24      N     payload                   N = payload_len bytes, opaque
24+N    32    hmac                      HMAC-SHA256 over bytes[0..24+N]
                                        (i.e. header + payload)
```

**总长度**：`24 + payload_len + 32` = 56 + payload_len 字节；最大 56 + 65536 = 65592 字节。

**版本升级策略**：wire version 从 1 起；若未来改 header layout，version bump 到 2；接收方拒绝未知版本。

- [ ] **该规范会在 Task 5 里落到 `docs/development/ipc-wire-format.md`**

---

## Task 1: FrameCodec — 序列化 / 反序列化

**Files:**
- Create (in Chromium src): `chrome/browser/exam/ipc/wire/frame_codec.h`
- Create (in Chromium src): `chrome/browser/exam/ipc/wire/frame_codec.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0620-add-frame-codec.patch`
- Create: `/root/byod-browser/build/smoke-test-p2b.sh`

**Interfaces:**
- Consumes: P2-A 全部完成（41 patch 已合入）；使用 P1-D 冻结的 `chrome::exam::GuardianFrame` + `chrome::exam::MessageType`
- **Produces**（P2-B Task 2/3 + P2-C/D/E 消费）：`chrome::exam::frame_codec::Encode` / `Decode` / `kMaxPayloadBytes` / `kHeaderBytes` / `kMagic`

- [ ] **Step 1: 起始状态自检**

Run:
```bash
cd /opt/chromium/src
git status
git describe --tags
git log --oneline | head -42
```
Expected: `working tree clean`；tag 一致；最近 41 条 commit 是 P1-A..P2-A 的补丁

若不干净：`echo y | bash /root/byod-browser/build/revert-patches.sh && bash /root/byod-browser/build/apply-patches.sh`

- [ ] **Step 2: 写 smoke-test-p2b.sh 骨架**

Write `/root/byod-browser/build/smoke-test-p2b.sh`:

```bash
#!/usr/bin/env bash
# Behavior smoke tests for P2-B patches (FrameCodec + HMAC + SequenceGuard).
# Real behavior lives in the gtest suite; this script only verifies files
# exist and Chromium still boots.
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

# ---------- Task 1: FrameCodec header ----------
echo "[test] P2-B/task1: FrameCodec"
if [ -f /opt/chromium/src/chrome/browser/exam/ipc/wire/frame_codec.h ]; then
  pass "frame_codec.h present"
else
  fail "frame_codec.h missing"
fi

# Verify chrome still boots
out="$(run_chrome --headless --dump-dom https://example.com 2>&1 || true)"
if echo "$out" | grep -q "Example Domain"; then
  pass "chrome boots after FrameCodec addition"
else
  fail "chrome broken after FrameCodec addition"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "[fail] one or more P2-B tests failed"; exit 1
fi
echo "[success] all P2-B tests passed"
```

Run:
```bash
chmod +x /root/byod-browser/build/smoke-test-p2b.sh
```

- [ ] **Step 3: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p2b.sh`
Expected: `✗ frame_codec.h missing`

- [ ] **Step 4: 创建 frame_codec.h**

Write `/opt/chromium/src/chrome/browser/exam/ipc/wire/frame_codec.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P2-B Task 1 (spec §5.1.3): pure serialization / deserialization
// for GuardianFrame. Wire format documented in
// docs/development/ipc-wire-format.md.
//
// Layout (little-endian):
//   [magic 4B]    0x42594F44  ('BYOD' LE)
//   [ver+type 4B] high 8b = version (v1); low 24b = MessageType
//   [seq 8B]      per-direction monotonic sequence
//   [len 4B]      payload length in bytes; ≤ 65536
//   [rsvd 4B]     reserved, must be 0
//   ---- header end (24 bytes) ----
//   [payload N]   opaque; N = len
//   [hmac 32B]    HMAC-SHA256 over bytes[0..24+N]
//
// Total frame length = 24 + N + 32.

#ifndef CHROME_BROWSER_EXAM_IPC_WIRE_FRAME_CODEC_H_
#define CHROME_BROWSER_EXAM_IPC_WIRE_FRAME_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "base/containers/span.h"
#include "chrome/browser/exam/ipc/guardian_transport.h"  // GuardianFrame

namespace chrome::exam::frame_codec {

// Wire limits & constants (per spec §5.1.3).
inline constexpr size_t kMaxPayloadBytes = 64 * 1024;  // 64 KiB
inline constexpr size_t kHeaderBytes = 24;
inline constexpr size_t kHmacBytes = 32;
inline constexpr size_t kMinFrameBytes = kHeaderBytes + kHmacBytes;  // 56
inline constexpr uint32_t kMagic = 0x42594F44u;
inline constexpr uint8_t kWireVersion = 1;

// Serialize |frame| to a fresh byte buffer. Returns nullopt if:
//   - frame.payload exceeds kMaxPayloadBytes
//   - frame.type has an unrepresentable MessageType value (upper 24 bits
//     of the type field don't fit)
//   - frame.hmac is not exactly kHmacBytes (or empty during pre-HMAC
//     encode; see EncodeUnsigned below).
//
// The written HMAC field is copied verbatim from frame.hmac. Callers
// that want a signed frame should use EncodeUnsigned + append the HMAC
// computed by frame_hmac::Compute (Task 2).
std::optional<std::vector<uint8_t>> Encode(const GuardianFrame& frame);

// Encode header + payload only (no HMAC trailer). Callers append the
// HMAC after computing it over the returned bytes.
std::optional<std::vector<uint8_t>> EncodeUnsigned(const GuardianFrame& frame);

// Deserialize |bytes| to a frame. Returns nullopt on any parse error:
//   - too short (< kMinFrameBytes)
//   - bad magic
//   - unknown version (!= kWireVersion)
//   - reserved bits nonzero
//   - payload_len > kMaxPayloadBytes
//   - total length mismatch (header.payload_len != bytes.size() - kMinFrameBytes)
//
// HMAC field is copied into frame.hmac verbatim; caller must call
// frame_hmac::Verify to check integrity (Task 2).
std::optional<GuardianFrame> Decode(base::span<const uint8_t> bytes);

}  // namespace chrome::exam::frame_codec

#endif  // CHROME_BROWSER_EXAM_IPC_WIRE_FRAME_CODEC_H_
```

- [ ] **Step 5: 创建 frame_codec.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/wire/frame_codec.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/ipc/wire/frame_codec.h"

#include <cstring>

#include "base/logging.h"

namespace chrome::exam::frame_codec {

namespace {

// Little-endian read/write helpers.
void WriteU32LE(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void WriteU64LE(uint8_t* out, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
  }
}

uint32_t ReadU32LE(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) |
         (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t ReadU64LE(const uint8_t* in) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) {
    v |= (static_cast<uint64_t>(in[i]) << (i * 8));
  }
  return v;
}

// Compose the ver+type field. Returns nullopt if type doesn't fit in
// 24 bits.
std::optional<uint32_t> ComposeVerType(MessageType type) {
  const uint32_t raw = static_cast<uint32_t>(type);
  if (raw > 0x00FFFFFFu) return std::nullopt;
  return (static_cast<uint32_t>(kWireVersion) << 24) | raw;
}

bool DecomposeVerType(uint32_t ver_type,
                      uint8_t* out_version,
                      uint32_t* out_type_raw) {
  *out_version = static_cast<uint8_t>((ver_type >> 24) & 0xFF);
  *out_type_raw = ver_type & 0x00FFFFFFu;
  return true;
}

void WriteHeader(uint8_t* out,
                 uint32_t ver_type,
                 uint64_t seq,
                 uint32_t payload_len) {
  WriteU32LE(out + 0, kMagic);
  WriteU32LE(out + 4, ver_type);
  WriteU64LE(out + 8, seq);
  WriteU32LE(out + 16, payload_len);
  WriteU32LE(out + 20, 0);  // reserved
}

}  // namespace

std::optional<std::vector<uint8_t>> EncodeUnsigned(const GuardianFrame& frame) {
  if (frame.payload.size() > kMaxPayloadBytes) {
    LOG(ERROR) << "[byod] FrameCodec: payload " << frame.payload.size()
               << " > max " << kMaxPayloadBytes;
    return std::nullopt;
  }
  auto ver_type = ComposeVerType(frame.type);
  if (!ver_type) {
    LOG(ERROR) << "[byod] FrameCodec: MessageType overflow";
    return std::nullopt;
  }

  std::vector<uint8_t> out(kHeaderBytes + frame.payload.size());
  WriteHeader(out.data(),
              *ver_type,
              frame.seq,
              static_cast<uint32_t>(frame.payload.size()));
  if (!frame.payload.empty()) {
    std::memcpy(out.data() + kHeaderBytes,
                frame.payload.data(),
                frame.payload.size());
  }
  return out;
}

std::optional<std::vector<uint8_t>> Encode(const GuardianFrame& frame) {
  if (frame.hmac.size() != kHmacBytes) {
    LOG(ERROR) << "[byod] FrameCodec::Encode: hmac size "
               << frame.hmac.size() << " != " << kHmacBytes;
    return std::nullopt;
  }
  auto unsigned_bytes = EncodeUnsigned(frame);
  if (!unsigned_bytes) return std::nullopt;

  unsigned_bytes->insert(unsigned_bytes->end(),
                         frame.hmac.begin(), frame.hmac.end());
  return unsigned_bytes;
}

std::optional<GuardianFrame> Decode(base::span<const uint8_t> bytes) {
  if (bytes.size() < kMinFrameBytes) {
    return std::nullopt;
  }

  const uint32_t magic = ReadU32LE(bytes.data());
  if (magic != kMagic) return std::nullopt;

  const uint32_t ver_type = ReadU32LE(bytes.data() + 4);
  uint8_t version = 0;
  uint32_t type_raw = 0;
  DecomposeVerType(ver_type, &version, &type_raw);
  if (version != kWireVersion) return std::nullopt;

  const uint64_t seq = ReadU64LE(bytes.data() + 8);
  const uint32_t payload_len = ReadU32LE(bytes.data() + 16);
  const uint32_t reserved = ReadU32LE(bytes.data() + 20);

  if (reserved != 0) return std::nullopt;
  if (payload_len > kMaxPayloadBytes) return std::nullopt;

  const size_t expected_total = kHeaderBytes + payload_len + kHmacBytes;
  if (bytes.size() != expected_total) return std::nullopt;

  GuardianFrame frame;
  frame.seq = seq;
  frame.type = static_cast<MessageType>(type_raw);
  frame.payload.assign(
      bytes.data() + kHeaderBytes,
      bytes.data() + kHeaderBytes + payload_len);
  frame.hmac.assign(
      bytes.data() + kHeaderBytes + payload_len,
      bytes.data() + expected_total);
  return frame;
}

}  // namespace chrome::exam::frame_codec
```

- [ ] **Step 6: 修改 BUILD.gn**

在 `/opt/chromium/src/chrome/browser/exam/BUILD.gn` 的 `source_set("exam")` 的 `sources = [ ... ]` 加入（保持字母序）：

```gn
    "ipc/wire/frame_codec.cc",
    "ipc/wire/frame_codec.h",
```

- [ ] **Step 7: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**若报 "GuardianFrame 未定义"**：检查 include 是否为 `"chrome/browser/exam/ipc/guardian_transport.h"`（P1-D 提供），路径是否正确。

- [ ] **Step 8: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
bash /root/byod-browser/build/smoke-test-p1c.sh
bash /root/byod-browser/build/smoke-test-p1d.sh
bash /root/byod-browser/build/smoke-test-p1e.sh
bash /root/byod-browser/build/smoke-test-p2a.sh
bash /root/byod-browser/build/smoke-test-p2b.sh
```

- [ ] **Step 9: 导出 patch + revert/re-apply 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/ipc/wire/frame_codec.h \
        chrome/browser/exam/ipc/wire/frame_codec.cc
git commit -m "byod: add FrameCodec (Encode/Decode) (P2-B/T1)

P2-B Task 1 (spec §5.1.3). Pure serialization for GuardianFrame using
little-endian fixed-size header + variable payload + HMAC trailer.
Wire format frozen at version 1; documented in ipc-wire-format.md.
Rejects malformed frames (bad magic, unknown version, nonzero reserved,
oversize payload, total length mismatch)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0620-add-frame-codec.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p2b.sh

cd /root/byod-browser
git add patches/0620-add-frame-codec.patch build/smoke-test-p2b.sh
git commit -m "patch: 0620 FrameCodec (P2-B/T1)

Est. LOC in Chromium: ~180."
```

---

## Task 2: FrameHmac — HMAC-SHA256 签名 & 校验

**Files:**
- Create (in Chromium src): `chrome/browser/exam/ipc/wire/frame_hmac.h`
- Create (in Chromium src): `chrome/browser/exam/ipc/wire/frame_hmac.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0621-add-frame-hmac.patch`

**Interfaces:**
- Consumes: Task 1 完成
- **Produces**（P2-C/D/E + P3/P4 消费）：`chrome::exam::frame_hmac::Compute` / `Verify`

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p2b.sh`，在 `if [ "$FAILED"` 之前追加：

```bash
# ---------- Task 2: FrameHmac header ----------
echo "[test] P2-B/task2: FrameHmac"
if [ -f /opt/chromium/src/chrome/browser/exam/ipc/wire/frame_hmac.h ]; then
  pass "frame_hmac.h present"
else
  fail "frame_hmac.h missing"
fi
```

- [ ] **Step 2: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p2b.sh`

- [ ] **Step 3: 创建 frame_hmac.h**

Write `/opt/chromium/src/chrome/browser/exam/ipc/wire/frame_hmac.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P2-B Task 2 (spec §5.1.3): HMAC-SHA256 signing and verification
// for GuardianFrame. Uses the IpcKey established by P2-A KeyChannel.
//
// The HMAC is computed over the encoded header+payload (i.e. the frame
// serialization *excluding* the final 32-byte HMAC trailer). This means
// the codec must serialize deterministically — see frame_codec.cc for
// the guarantee.

#ifndef CHROME_BROWSER_EXAM_IPC_WIRE_FRAME_HMAC_H_
#define CHROME_BROWSER_EXAM_IPC_WIRE_FRAME_HMAC_H_

#include <cstdint>
#include <vector>

#include "base/containers/span.h"
#include "chrome/browser/exam/ipc/guardian_transport.h"  // GuardianFrame
#include "chrome/browser/exam/ipc/handshake/key_channel.h"  // IpcKey

namespace chrome::exam::frame_hmac {

// Compute HMAC-SHA256 over |encoded_header_and_payload| using |key|.
// Always returns 32 bytes. Non-throwing.
std::vector<uint8_t> Compute(
    const IpcKey& key,
    base::span<const uint8_t> encoded_header_and_payload);

// Verify that |frame.hmac| matches the HMAC computed over the encoded
// header+payload of |frame|. Constant-time comparison. Returns false if:
//   - frame.hmac.size() != 32
//   - the encode fails
//   - the computed HMAC does not match frame.hmac
bool Verify(const IpcKey& key, const GuardianFrame& frame);

}  // namespace chrome::exam::frame_hmac

#endif  // CHROME_BROWSER_EXAM_IPC_WIRE_FRAME_HMAC_H_
```

- [ ] **Step 4: 创建 frame_hmac.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/wire/frame_hmac.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/ipc/wire/frame_hmac.h"

#include "base/logging.h"
#include "chrome/browser/exam/ipc/wire/frame_codec.h"
#include "crypto/hmac.h"

namespace chrome::exam::frame_hmac {

std::vector<uint8_t> Compute(
    const IpcKey& key,
    base::span<const uint8_t> encoded_header_and_payload) {
  crypto::HMAC hmac(crypto::HMAC::SHA256);
  const bool init_ok = hmac.Init(key.bytes.data(), key.bytes.size());
  CHECK(init_ok) << "[byod] HMAC Init failed";  // 256-bit key always valid

  std::vector<uint8_t> tag(hmac.DigestLength());
  const bool sign_ok = hmac.Sign(
      base::as_string_view(encoded_header_and_payload),
      tag.data(),
      tag.size());
  CHECK(sign_ok) << "[byod] HMAC Sign failed";
  return tag;
}

bool Verify(const IpcKey& key, const GuardianFrame& frame) {
  if (frame.hmac.size() != frame_codec::kHmacBytes) {
    return false;
  }
  auto encoded = frame_codec::EncodeUnsigned(frame);
  if (!encoded) return false;

  crypto::HMAC hmac(crypto::HMAC::SHA256);
  if (!hmac.Init(key.bytes.data(), key.bytes.size())) return false;

  // crypto::HMAC::Verify uses constant-time comparison per Chromium docs.
  return hmac.Verify(
      base::as_string_view(base::span<const uint8_t>(*encoded)),
      base::as_string_view(base::span<const uint8_t>(frame.hmac)));
}

}  // namespace chrome::exam::frame_hmac
```

**注意**：
- `crypto::HMAC` 是 Chromium 已有类；`crypto::HMAC::SHA256` 是 enum
- `base::as_string_view` 把 `base::span<const uint8_t>` 转成 `std::string_view` 供 crypto API 使用（新版 Chromium 首选 API）；老版本用 `base::StringPiece(reinterpret_cast<const char*>(data), size)`
- `crypto::HMAC::Verify` 是 constant-time 的（Chromium 文档保证），避免 timing side-channel

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "ipc/wire/frame_hmac.cc",
    "ipc/wire/frame_hmac.h",
```

deps 确认有 `"//crypto"`（P2-A 已加过，重复无害）：

```gn
  deps += [ "//crypto" ]
```

- [ ] **Step 6: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**常见报错**：
- `crypto::HMAC::Sign` 参数类型不匹配：新版 API 用 `base::span<uint8_t>` 输出而非 raw pointer；按 header 声明调整
- `base::as_string_view` 未定义：改用 `base::StringPiece(reinterpret_cast<const char*>(span.data()), span.size())`

- [ ] **Step 7: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p2b.sh
```

- [ ] **Step 8: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/ipc/wire/frame_hmac.h \
        chrome/browser/exam/ipc/wire/frame_hmac.cc
git commit -m "byod: add FrameHmac (Compute/Verify) (P2-B/T2)

P2-B Task 2 (spec §5.1.3). HMAC-SHA256 over encoded header+payload
using IpcKey. Verify uses crypto::HMAC::Verify which is constant-time
per Chromium docs. Sign path CHECKs on init failure since IpcKey is
guaranteed 32 bytes."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0621-add-frame-hmac.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p2b.sh

cd /root/byod-browser
git add patches/0621-add-frame-hmac.patch build/smoke-test-p2b.sh
git commit -m "patch: 0621 FrameHmac (P2-B/T2)

Est. LOC in Chromium: ~70."
```

---

## Task 3: SequenceGuard — 单调序号防重放

**Files:**
- Create (in Chromium src): `chrome/browser/exam/ipc/wire/sequence_guard.h`
- Create (in Chromium src): `chrome/browser/exam/ipc/wire/sequence_guard.cc`
- Modify (in Chromium src): `chrome/browser/exam/BUILD.gn`
- Create: `/root/byod-browser/patches/0622-add-sequence-guard.patch`

**Interfaces:**
- Consumes: Task 2 完成
- **Produces**（P2-C/D/E 消费）：`chrome::exam::SequenceGuard` 类

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p2b.sh`：

```bash
# ---------- Task 3: SequenceGuard header ----------
echo "[test] P2-B/task3: SequenceGuard"
if [ -f /opt/chromium/src/chrome/browser/exam/ipc/wire/sequence_guard.h ]; then
  pass "sequence_guard.h present"
else
  fail "sequence_guard.h missing"
fi
```

- [ ] **Step 2: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p2b.sh`

- [ ] **Step 3: 创建 sequence_guard.h**

Write `/opt/chromium/src/chrome/browser/exam/ipc/wire/sequence_guard.h`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P2-B Task 3 (spec §5.1.3): monotonic per-direction sequence
// number guard against replay attacks.
//
// A SequenceGuard instance is used per direction: Browser→Guardian keeps
// one, Guardian→Browser keeps another. AcceptAndAdvance rejects seq that
// is not strictly greater than the highest previously-accepted seq,
// preventing an attacker from replaying old frames captured on the wire.
//
// Not thread-safe: caller must ensure single-threaded access (in P2-E
// the read loop and write loop each own their own instance and run on
// their own sequence).

#ifndef CHROME_BROWSER_EXAM_IPC_WIRE_SEQUENCE_GUARD_H_
#define CHROME_BROWSER_EXAM_IPC_WIRE_SEQUENCE_GUARD_H_

#include <cstdint>

namespace chrome::exam {

class SequenceGuard {
 public:
  SequenceGuard() = default;

  // Return true and mark |seq| as seen iff seq > highest_seen(). Return
  // false without state change on replay / regression.
  bool AcceptAndAdvance(uint64_t seq);

  // Increment and return the next sequence number for outbound frames.
  // First call returns 1.
  uint64_t NextOutbound();

  uint64_t highest_seen() const { return highest_; }

 private:
  uint64_t highest_ = 0;
};

}  // namespace chrome::exam

#endif  // CHROME_BROWSER_EXAM_IPC_WIRE_SEQUENCE_GUARD_H_
```

- [ ] **Step 4: 创建 sequence_guard.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/wire/sequence_guard.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "chrome/browser/exam/ipc/wire/sequence_guard.h"

namespace chrome::exam {

bool SequenceGuard::AcceptAndAdvance(uint64_t seq) {
  if (seq <= highest_) return false;
  highest_ = seq;
  return true;
}

uint64_t SequenceGuard::NextOutbound() {
  return ++highest_;
}

}  // namespace chrome::exam
```

**说明**：
- `NextOutbound` 借用 `highest_` 作为发送 counter；这在**单向**使用（一个方向一个实例）时正确
- 若代码里意外把同一个 SequenceGuard 既用于发也用于收，会因为 highest_ 被 `AcceptAndAdvance` 意外推高，`NextOutbound` 会跳号 —— **文档明确一实例一方向**，acceptance 里的 gtest 会 sanity check

- [ ] **Step 5: 修改 BUILD.gn**

Sources 加：

```gn
    "ipc/wire/sequence_guard.cc",
    "ipc/wire/sequence_guard.h",
```

- [ ] **Step 6: 编译 + smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p2b.sh
```

- [ ] **Step 7: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/BUILD.gn \
        chrome/browser/exam/ipc/wire/sequence_guard.h \
        chrome/browser/exam/ipc/wire/sequence_guard.cc
git commit -m "byod: add SequenceGuard (P2-B/T3)

P2-B Task 3 (spec §5.1.3). Monotonic per-direction sequence number
guard. AcceptAndAdvance rejects replay/regression; NextOutbound
generates monotonic outbound seq. Not thread-safe; one instance per
direction (see file comment)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0622-add-sequence-guard.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p2b.sh

cd /root/byod-browser
git add patches/0622-add-sequence-guard.patch build/smoke-test-p2b.sh
git commit -m "patch: 0622 SequenceGuard (P2-B/T3)

Est. LOC in Chromium: ~40."
```

---

## Task 4: gtest 单元测试 — codec + hmac + seq guard

**Files:**
- Create (in Chromium src): `chrome/browser/exam/ipc/wire/frame_codec_unittest.cc`
- Create (in Chromium src): `chrome/browser/exam/ipc/wire/frame_hmac_unittest.cc`
- Create (in Chromium src): `chrome/browser/exam/ipc/wire/sequence_guard_unittest.cc`
- Modify (in Chromium src): `chrome/test/BUILD.gn` — 加 3 个 unittest 到 unit_tests target
- Create: `/root/byod-browser/patches/0623-add-frame-wire-unit-tests.patch`

**Interfaces:**
- Consumes: Task 3 完成
- Produces: gtest suite `FrameCodecTest.*` / `FrameHmacTest.*` / `SequenceGuardTest.*` 全部通过

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p2b.sh`：

```bash
# ---------- Task 4: unit tests present ----------
echo "[test] P2-B/task4: wire unit tests"
for f in \
    /opt/chromium/src/chrome/browser/exam/ipc/wire/frame_codec_unittest.cc \
    /opt/chromium/src/chrome/browser/exam/ipc/wire/frame_hmac_unittest.cc \
    /opt/chromium/src/chrome/browser/exam/ipc/wire/sequence_guard_unittest.cc; do
  if [ -f "$f" ]; then pass "$(basename "$f") present"; else fail "$(basename "$f") missing"; fi
done
```

- [ ] **Step 2: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p2b.sh`

- [ ] **Step 3: 创建 frame_codec_unittest.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/wire/frame_codec_unittest.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P2-B Task 4: FrameCodec unit tests.

#include "chrome/browser/exam/ipc/wire/frame_codec.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace chrome::exam::frame_codec {

namespace {

GuardianFrame MakeSignedFrame(uint64_t seq, MessageType type,
                              std::vector<uint8_t> payload) {
  GuardianFrame f;
  f.seq = seq;
  f.type = type;
  f.payload = std::move(payload);
  f.hmac.assign(kHmacBytes, 0xAA);  // dummy 32-byte "hmac"
  return f;
}

}  // namespace

TEST(FrameCodecTest, EncodeDecodeRoundTrip_EmptyPayload) {
  auto original = MakeSignedFrame(42, MessageType::kHeartbeat, {});
  auto encoded = Encode(original);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(kMinFrameBytes, encoded->size());

  auto decoded = Decode(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(original.seq, decoded->seq);
  EXPECT_EQ(original.type, decoded->type);
  EXPECT_EQ(original.payload, decoded->payload);
  EXPECT_EQ(original.hmac, decoded->hmac);
}

TEST(FrameCodecTest, EncodeDecodeRoundTrip_LargePayload) {
  std::vector<uint8_t> payload(1024, 0x77);
  auto original = MakeSignedFrame(0xDEADBEEFDEADBEEFULL,
                                  MessageType::kEvent,
                                  std::move(payload));
  auto encoded = Encode(original);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(kMinFrameBytes + 1024u, encoded->size());

  auto decoded = Decode(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(original.seq, decoded->seq);
  EXPECT_EQ(original.type, decoded->type);
  EXPECT_EQ(original.payload, decoded->payload);
  EXPECT_EQ(original.hmac, decoded->hmac);
}

TEST(FrameCodecTest, EncodeRejectsOversizePayload) {
  std::vector<uint8_t> payload(kMaxPayloadBytes + 1, 0x00);
  auto frame = MakeSignedFrame(1, MessageType::kEvent, std::move(payload));
  EXPECT_FALSE(Encode(frame).has_value());
}

TEST(FrameCodecTest, EncodeRejectsWrongHmacSize) {
  auto frame = MakeSignedFrame(1, MessageType::kHeartbeat, {});
  frame.hmac.assign(10, 0);  // wrong size
  EXPECT_FALSE(Encode(frame).has_value());
}

TEST(FrameCodecTest, DecodeRejectsShortBuffer) {
  std::vector<uint8_t> too_short(kMinFrameBytes - 1, 0);
  EXPECT_FALSE(Decode(too_short).has_value());
}

TEST(FrameCodecTest, DecodeRejectsBadMagic) {
  auto original = MakeSignedFrame(1, MessageType::kHeartbeat, {});
  auto encoded = Encode(original);
  ASSERT_TRUE(encoded.has_value());
  (*encoded)[0] ^= 0xFF;  // Corrupt magic
  EXPECT_FALSE(Decode(*encoded).has_value());
}

TEST(FrameCodecTest, DecodeRejectsBadVersion) {
  auto original = MakeSignedFrame(1, MessageType::kHeartbeat, {});
  auto encoded = Encode(original);
  ASSERT_TRUE(encoded.has_value());
  // Byte at offset 7 holds the high 8 bits of ver_type = version byte.
  (*encoded)[7] = 99;  // Unknown version
  EXPECT_FALSE(Decode(*encoded).has_value());
}

TEST(FrameCodecTest, DecodeRejectsNonzeroReserved) {
  auto original = MakeSignedFrame(1, MessageType::kHeartbeat, {});
  auto encoded = Encode(original);
  ASSERT_TRUE(encoded.has_value());
  // Reserved is at bytes [20..24). Corrupt one bit.
  (*encoded)[20] = 0x01;
  EXPECT_FALSE(Decode(*encoded).has_value());
}

TEST(FrameCodecTest, DecodeRejectsTotalLengthMismatch) {
  auto original = MakeSignedFrame(1, MessageType::kHeartbeat, {0x01, 0x02});
  auto encoded = Encode(original);
  ASSERT_TRUE(encoded.has_value());
  encoded->push_back(0xFF);  // Extra trailing byte
  EXPECT_FALSE(Decode(*encoded).has_value());
}

TEST(FrameCodecTest, EncodeUnsignedThenAppendHmac) {
  auto frame = MakeSignedFrame(1, MessageType::kHeartbeat, {0xAA, 0xBB});
  // Save the hmac from the "signed" frame, clear it, and re-derive via
  // EncodeUnsigned + append.
  std::vector<uint8_t> saved_hmac = frame.hmac;
  GuardianFrame unsigned_frame = frame;
  unsigned_frame.hmac.clear();

  auto unsigned_bytes = EncodeUnsigned(unsigned_frame);
  ASSERT_TRUE(unsigned_bytes.has_value());
  EXPECT_EQ(kHeaderBytes + 2u, unsigned_bytes->size());

  // Compare with Encode's header+payload portion.
  auto full_bytes = Encode(frame);
  ASSERT_TRUE(full_bytes.has_value());
  EXPECT_EQ(unsigned_bytes->size() + kHmacBytes, full_bytes->size());
  EXPECT_TRUE(std::equal(unsigned_bytes->begin(), unsigned_bytes->end(),
                          full_bytes->begin()));
}

}  // namespace chrome::exam::frame_codec
```

- [ ] **Step 4: 创建 frame_hmac_unittest.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/wire/frame_hmac_unittest.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P2-B Task 4: FrameHmac unit tests.

#include "chrome/browser/exam/ipc/wire/frame_hmac.h"

#include "chrome/browser/exam/ipc/wire/frame_codec.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chrome::exam::frame_hmac {

namespace {

IpcKey MakeDeterministicKey() {
  IpcKey k;
  for (size_t i = 0; i < k.bytes.size(); ++i) {
    k.bytes[i] = static_cast<uint8_t>(i);
  }
  return k;
}

GuardianFrame MakeFrame(uint64_t seq, MessageType type,
                        std::vector<uint8_t> payload) {
  GuardianFrame f;
  f.seq = seq;
  f.type = type;
  f.payload = std::move(payload);
  return f;
}

}  // namespace

TEST(FrameHmacTest, ComputeReturns32Bytes) {
  IpcKey key = MakeDeterministicKey();
  auto frame = MakeFrame(1, MessageType::kHeartbeat, {});
  auto encoded = frame_codec::EncodeUnsigned(frame);
  ASSERT_TRUE(encoded.has_value());

  auto tag = Compute(key, *encoded);
  EXPECT_EQ(32u, tag.size());
}

TEST(FrameHmacTest, ComputeIsDeterministicForSameInput) {
  IpcKey key = MakeDeterministicKey();
  auto frame = MakeFrame(42, MessageType::kEvent, {0x11, 0x22, 0x33});
  auto encoded = frame_codec::EncodeUnsigned(frame);
  ASSERT_TRUE(encoded.has_value());

  auto tag1 = Compute(key, *encoded);
  auto tag2 = Compute(key, *encoded);
  EXPECT_EQ(tag1, tag2);
}

TEST(FrameHmacTest, DifferentKeyProducesDifferentTag) {
  IpcKey key1 = MakeDeterministicKey();
  IpcKey key2 = MakeDeterministicKey();
  key2.bytes[0] ^= 0xFF;  // one-bit diff

  auto frame = MakeFrame(1, MessageType::kHeartbeat, {});
  auto encoded = frame_codec::EncodeUnsigned(frame);
  ASSERT_TRUE(encoded.has_value());

  auto tag1 = Compute(key1, *encoded);
  auto tag2 = Compute(key2, *encoded);
  EXPECT_NE(tag1, tag2);
}

TEST(FrameHmacTest, VerifyRoundTrip) {
  IpcKey key = MakeDeterministicKey();
  auto frame = MakeFrame(7, MessageType::kChallenge, {0xAB, 0xCD});
  auto encoded = frame_codec::EncodeUnsigned(frame);
  ASSERT_TRUE(encoded.has_value());
  frame.hmac = Compute(key, *encoded);

  EXPECT_TRUE(Verify(key, frame));
}

TEST(FrameHmacTest, VerifyRejectsTamperedPayload) {
  IpcKey key = MakeDeterministicKey();
  auto frame = MakeFrame(7, MessageType::kEvent, {0x01, 0x02});
  auto encoded = frame_codec::EncodeUnsigned(frame);
  ASSERT_TRUE(encoded.has_value());
  frame.hmac = Compute(key, *encoded);

  frame.payload[0] ^= 0x01;  // tamper
  EXPECT_FALSE(Verify(key, frame));
}

TEST(FrameHmacTest, VerifyRejectsWrongHmacSize) {
  IpcKey key = MakeDeterministicKey();
  auto frame = MakeFrame(1, MessageType::kHeartbeat, {});
  frame.hmac.assign(31, 0);  // wrong size
  EXPECT_FALSE(Verify(key, frame));
}

}  // namespace chrome::exam::frame_hmac
```

- [ ] **Step 5: 创建 sequence_guard_unittest.cc**

Write `/opt/chromium/src/chrome/browser/exam/ipc/wire/sequence_guard_unittest.cc`:

```cpp
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P2-B Task 4: SequenceGuard unit tests.

#include "chrome/browser/exam/ipc/wire/sequence_guard.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace chrome::exam {

TEST(SequenceGuardTest, InitialHighestIsZero) {
  SequenceGuard g;
  EXPECT_EQ(0u, g.highest_seen());
}

TEST(SequenceGuardTest, AcceptStrictlyIncreasing) {
  SequenceGuard g;
  EXPECT_TRUE(g.AcceptAndAdvance(1));
  EXPECT_TRUE(g.AcceptAndAdvance(2));
  EXPECT_TRUE(g.AcceptAndAdvance(1000));
  EXPECT_EQ(1000u, g.highest_seen());
}

TEST(SequenceGuardTest, RejectReplay) {
  SequenceGuard g;
  EXPECT_TRUE(g.AcceptAndAdvance(5));
  EXPECT_FALSE(g.AcceptAndAdvance(5));   // exact replay
  EXPECT_FALSE(g.AcceptAndAdvance(3));   // regression
  EXPECT_EQ(5u, g.highest_seen());
}

TEST(SequenceGuardTest, GapsAccepted) {
  SequenceGuard g;
  EXPECT_TRUE(g.AcceptAndAdvance(100));
  EXPECT_TRUE(g.AcceptAndAdvance(101));
  // Jump forward (Guardian sent 102-999 which we didn't receive; still OK).
  EXPECT_TRUE(g.AcceptAndAdvance(1000));
  EXPECT_EQ(1000u, g.highest_seen());
}

TEST(SequenceGuardTest, NextOutboundIsMonotonic) {
  SequenceGuard g;
  EXPECT_EQ(1u, g.NextOutbound());
  EXPECT_EQ(2u, g.NextOutbound());
  EXPECT_EQ(3u, g.NextOutbound());
}

TEST(SequenceGuardTest, MixedInboundOutboundWarning) {
  // This test documents the "one instance per direction" contract by
  // showing what happens if the contract is violated: NextOutbound jumps
  // past inbound-accepted values. Callers should NOT mix; a comment in
  // the header spells this out.
  SequenceGuard g;
  EXPECT_TRUE(g.AcceptAndAdvance(50));
  EXPECT_EQ(51u, g.NextOutbound());  // Jumped from 50, not 1 — misuse.
}

}  // namespace chrome::exam
```

- [ ] **Step 6: 修改 chrome/test/BUILD.gn 加 3 个 unittest**

在 `unit_tests` target 的 `sources = [ ... ]` 加入：

```gn
    "../browser/exam/ipc/wire/frame_codec_unittest.cc",
    "../browser/exam/ipc/wire/frame_hmac_unittest.cc",
    "../browser/exam/ipc/wire/sequence_guard_unittest.cc",
```

- [ ] **Step 7: 编译 + 跑 gtest**

Run:
```bash
autoninja -C /opt/chromium/src/out/linux unit_tests
/opt/chromium/src/out/linux/unit_tests \
  --gtest_filter="FrameCodecTest.*:FrameHmacTest.*:SequenceGuardTest.*" \
  2>&1 | tail -30
```
Expected: 全部 pass；输出末尾类似 `[  PASSED  ] 21 tests.`

- [ ] **Step 8: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p2b.sh
```

- [ ] **Step 9: 导出 patch + revert/re-apply 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/exam/ipc/wire/frame_codec_unittest.cc \
        chrome/browser/exam/ipc/wire/frame_hmac_unittest.cc \
        chrome/browser/exam/ipc/wire/sequence_guard_unittest.cc \
        chrome/test/BUILD.gn
git commit -m "byod: add wire/codec/hmac/seq gtest suite (P2-B/T4)

P2-B Task 4. FrameCodec (10 tests) + FrameHmac (6 tests) + SequenceGuard
(6 tests). Covers round-trip, tampering rejection, malformed rejection,
wire-format spec adherence, and the 'one instance per direction'
contract for SequenceGuard."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0623-add-frame-wire-unit-tests.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
autoninja -C /opt/chromium/src/out/linux unit_tests
/opt/chromium/src/out/linux/unit_tests \
  --gtest_filter="FrameCodecTest.*:FrameHmacTest.*:SequenceGuardTest.*" \
  2>&1 | tail -5

cd /root/byod-browser
git add patches/0623-add-frame-wire-unit-tests.patch build/smoke-test-p2b.sh
git commit -m "patch: 0623 wire unit tests (P2-B/T4)

Est. LOC in Chromium: ~280 (22 gtests)."
```

---

## Task 5: 冻结 wire format 文档

**Files:**
- Create: `/root/byod-browser/docs/development/ipc-wire-format.md`

**Interfaces:**
- Consumes: Task 1-4 完成
- Produces: 给 Rust (P3) 和 Swift (P4) Guardian 实现方的**规范文档** —— 他们不再需要 read Chromium C++ 代码就能实现 wire codec

- [ ] **Step 1: 写规范文档**

Write `/root/byod-browser/docs/development/ipc-wire-format.md`:

```markdown
# BYOD IPC Wire Format (v1)

**Frozen at**: 2026-07-12 (P2-B Task 5)
**Implementations**:
- Chromium C++ (Browser side): `chrome/browser/exam/ipc/wire/frame_codec.{h,cc}`
- Guardian Rust (Windows): P3 will implement per this spec
- Guardian Swift (macOS): P4 will implement per this spec

**Design principle**: fixed-size header + variable payload + trailer HMAC.
Little-endian throughout. No dependency on protobuf / flatbuffers /
serde beyond byte-level primitives so any language can produce a
compatible encoder in ~200 LOC.

## Frame Layout

Total length = `24 + payload_len + 32` bytes.

| Offset | Size | Field              | Notes |
|--------|------|--------------------|-------|
| 0      | 4    | `magic`            | LE `0x42594F44` (== "BYOD" LE) |
| 4      | 4    | `version_and_type` | high 8 bits = version, low 24 = type |
| 8      | 8    | `seq`              | LE uint64, monotonic per direction |
| 16     | 4    | `payload_len`      | LE uint32, ≤ 65536 |
| 20     | 4    | `reserved`         | LE uint32, MUST be 0 |
| 24     | N    | `payload`          | opaque; N = payload_len |
| 24+N   | 32   | `hmac`             | HMAC-SHA256 over bytes[0..24+N] |

### version_and_type breakdown

Read the 4-byte LE uint32; then:

- `version = (value >> 24) & 0xFF`  → currently **must be 1**
- `type_raw = value & 0x00FFFFFF`   → cast to `MessageType` enum

If a receiver sees `version != 1` it MUST drop the frame and disconnect.

### MessageType enum values

Defined in Chromium `chrome/browser/exam/ipc/guardian_transport.h`:

| Value  | Symbol              | Direction |
|--------|---------------------|-----------|
| 1      | kHandshakeHello     | Guardian → Browser |
| 2      | kHandshakeAck       | Browser → Guardian |
| 10     | kHeartbeat          | both      |
| 20     | kChallenge          | Browser → Guardian |
| 21     | kChallengeResponse  | Guardian → Browser |
| 30     | kEvent              | Guardian → Browser |
| 40     | kBurnCommand        | Browser → Guardian |

Future values MUST be added at the end; do not reuse.

## HMAC-SHA256

- **Key**: 32 bytes shared secret from KeyChannel handshake (P2-A).
- **Message**: `bytes[0..24+N]` — the entire header + payload.
- **Tag**: appended as 32 bytes after payload.
- **Verification**: constant-time byte comparison. On mismatch the
  receiver MUST drop the frame and treat as a soft error (not a
  disconnect — could be a corrupt kernel buffer). Repeated mismatches
  (>3 in 10 seconds) SHOULD escalate to disconnect.

## Sequence Numbers

- Per direction. Sender maintains a counter starting at 0; each outbound
  frame increments and uses the new value.
- Receiver rejects any `seq <= highest_seen`.
- Sequence numbers restart at 0 on every new handshake (fresh session).

## Handshake

1. Browser fork Guardian with KeyChannel child end inherited.
2. Guardian reads 32 bytes from the inherited end → this is the IPC key.
3. Guardian sends `kHandshakeHello` frame (seq=1, empty payload, valid HMAC).
4. Browser verifies HMAC + accepts seq=1 → sends `kHandshakeAck` (seq=1, empty
   payload).
5. Guardian verifies ack → begins heartbeat + challenge loop (P2-C, P2-D).

If any step fails within 5 seconds of Guardian launch, both sides abort.

## Payload Schemas per MessageType

Payloads are **opaque byte strings** at the wire level. Interpretation
depends on `MessageType`:

| Type | Payload encoding | Documented in |
|------|------------------|---------------|
| kHandshakeHello / kHandshakeAck | empty (0 bytes) | this doc |
| kHeartbeat | 0 or JSON object with liveness metadata | P2-C |
| kChallenge | JSON with challenge nonce + type | P2-D |
| kChallengeResponse | JSON with signed response | P2-D |
| kEvent | JSON representation of `ExamEvent` (P1-D) | P8 |
| kBurnCommand | JSON with `reason` string | P2-E |

Payload JSON details are frozen in their respective phases; wire format
is version-independent from payload schema.

## Version Bump Procedure

If we ever need to change header layout:

1. Bump `kWireVersion` in `frame_codec.h` to 2.
2. Update Guardian implementations to accept both 1 and 2 during rollout.
3. After all deployed Browsers upgraded, drop version 1 support in Guardian.
4. Update this document with a "History" section noting v1→v2 diff.

## History

- **v1** (2026-07-12): initial layout above. Frozen for BYOD Exam Browser v1.0.
```

- [ ] **Step 2: Commit**

Run:
```bash
cd /root/byod-browser
git add docs/development/ipc-wire-format.md
git commit -m "docs: freeze IPC wire format v1 for P3/P4 Guardian implementations

P2-B Task 5. Full byte-level spec so Rust (Windows Guardian) and Swift
(macOS Guardian) can implement compatible codec without reading Chromium
C++ source. Includes MessageType enum table, HMAC verification rules,
sequence number semantics, handshake flow, and version bump procedure."
```

---

## Task 6: P2-B 交付验收清单

**Files:**
- Create: `/root/byod-browser/docs/development/p2-b-acceptance.md`

**Interfaces:**
- Consumes: Task 1..5 全部完成
- Produces: P2-B 交付验收 checklist

- [ ] **Step 1: 统计当前 patch 总行数**

Run:
```bash
source /root/byod-browser/build/chromium-version.env
cd /opt/chromium/src
git diff --shortstat "$CHROMIUM_TAG"..HEAD
```
Expected: P1 + P2-A + P2-B 累计 insertions；实际 Chromium 补丁行数（改 Chromium 现有代码）仍约 2795-2810 行（BUILD.gn 增量），本 sub-plan 新目录不占预算

- [ ] **Step 2: 数累计补丁文件数**

Run:
```bash
ls /root/byod-browser/patches/*.patch | wc -l
```
Expected: **45**（P1 34 + P2-A 7 + P2-B 4）

- [ ] **Step 3: 全量 revert + apply + 编译 + 全量 smoke + 全量 gtest**

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
bash /root/byod-browser/build/smoke-test-p2a.sh
bash /root/byod-browser/build/smoke-test-p2b.sh
autoninja -C /opt/chromium/src/out/linux unit_tests
/opt/chromium/src/out/linux/unit_tests \
  --gtest_filter="IpcKeyTest.*:KeyChannelTest.*:KeyChannelMultiProcessTest.*:PeerAttestorTest.*:FrameCodecTest.*:FrameHmacTest.*:SequenceGuardTest.*" \
  2>&1 | tail -10
```
Expected: 全部成功

- [ ] **Step 4: 写 acceptance checklist**

Write `/root/byod-browser/docs/development/p2-b-acceptance.md`:

```markdown
# P2-B 交付验收 Checklist

## 补丁类

- [ ] `patches/0620-add-frame-codec.patch` 存在
- [ ] `patches/0621-add-frame-hmac.patch` 存在
- [ ] `patches/0622-add-sequence-guard.patch` 存在
- [ ] `patches/0623-add-frame-wire-unit-tests.patch` 存在

## 编译类

- [ ] `bash build/revert-patches.sh && bash build/apply-patches.sh` 无冲突
- [ ] `bash build/build-linux.sh chrome` 成功
- [ ] `autoninja -C out/linux unit_tests` 成功

## 行为类（Linux）

- [ ] `bash build/smoke-test.sh` + `smoke-test-p1a..e.sh` + `smoke-test-p2a.sh` + `smoke-test-p2b.sh` 全 pass
- [ ] `unit_tests --gtest_filter="FrameCodecTest.*"`：10 个 test 全 PASS
- [ ] `unit_tests --gtest_filter="FrameHmacTest.*"`：6 个 test 全 PASS
- [ ] `unit_tests --gtest_filter="SequenceGuardTest.*"`：6 个 test 全 PASS

## 接口冻结类（供 P2-C/D/E + P3/P4 消费）

- [ ] `chrome::exam::frame_codec::Encode` / `EncodeUnsigned` / `Decode` 已 export
- [ ] `chrome::exam::frame_codec::kMagic` / `kMaxPayloadBytes` / `kHeaderBytes` / `kHmacBytes` / `kMinFrameBytes` / `kWireVersion` 常量已 export
- [ ] `chrome::exam::frame_hmac::Compute` / `Verify` 已 export
- [ ] `chrome::exam::SequenceGuard` 类已 export
- [ ] wire format 规范文档 `docs/development/ipc-wire-format.md` 存在且冻结

## 补丁行数 / 预算类

- [ ] 累计补丁：45（P1 34 + P2-A 7 + P2-B 4）
- [ ] Chromium 现有代码修改（BUILD.gn 行数） ≤ 30 行；本 sub-plan 全部新文件在 `chrome/browser/exam/ipc/wire/` 新目录
- [ ] 累计"改 Chromium 现有代码"净增 ≤ 3000 行硬预算（spec §3.1）

## 文档类

- [ ] 本文件（p2-b-acceptance.md）存在
- [ ] `docs/development/ipc-wire-format.md` 存在
- [ ] roadmap 里 P2 状态更新为 `⏳ 进行中 (P2-A + P2-B 完成)`

## 交付判定

以上全部打勾 → P2-B 完成，可进入 P2-C（尚未编写）。

**下一里程碑**：P2-C 完成后 Guardian 侧（P3/P4）可以开始 —— 至此 wire format 冻结、密钥握手就绪、HMAC 校验就绪、心跳协议就绪。
```

- [ ] **Step 5: Commit acceptance + 更新 roadmap**

Run:
```bash
cd /root/byod-browser
git add docs/development/p2-b-acceptance.md
git commit -m "docs: add P2-B acceptance checklist"
```

编辑 `/root/byod-browser/docs/superpowers/plans/2026-07-11-byod-roadmap.md` 把 P2 状态改成 `⏳ 进行中 (P2-A + P2-B 完成)`，然后：

```bash
cd /root/byod-browser
git add docs/superpowers/plans/2026-07-11-byod-roadmap.md
git commit -m "docs: mark P2-B complete in roadmap"
```

---

## P2-B 完成后

**已交付 4 个补丁文件**：
- `0620-add-frame-codec.patch` — 序列化/反序列化 + wire format 常量
- `0621-add-frame-hmac.patch` — HMAC-SHA256 signing & verification
- `0622-add-sequence-guard.patch` — 单调序号防重放
- `0623-add-frame-wire-unit-tests.patch` — 22 个 gtest 全覆盖

**接口冻结完成**（供 P2-C/D/E + P3/P4 消费）：
- `frame_codec::Encode` / `Decode` + wire format 常量
- `frame_hmac::Compute` / `Verify`
- `SequenceGuard`（AcceptAndAdvance / NextOutbound）
- **wire format v1 规范文档冻结**（`docs/development/ipc-wire-format.md`）—— Rust/Swift Guardian 侧无需读 C++ 代码即可实现兼容 codec

**下一批 P2-C**：心跳 driver —— `base::RepeatingTimer` 5s 触发发心跳 + tick 状态机 + `PowerObserver` 订阅 sleep/wake 事件。预算 ~500 行。

**预计 P2-B 实际耗时**：
- 熟练 Chromium 开发者：2-3 天
- 首次改 Chromium 的开发者：4-6 天

**风险与常见坑**：
- **`crypto::HMAC` API 在新版 Chromium 变化**：可能不再接受 `std::string_view`，改用 `base::span<uint8_t>`；按报错调整
- **`base::as_string_view` 未定义**：老版 Chromium 不用这个 helper；改用 `base::StringPiece(reinterpret_cast<const char*>(...), size)`
- **`base::span` include 路径**：新版 `base/containers/span.h`；老版可能是 `base/containers/span.h` 但 API 微差；按 header 报错调整
- **wire format 端序错误**：本 sub-plan 全 little-endian，若 Rust/Swift 实现方忘记明确指定用 native endianness（x86/ARM64 是 LE 巧合正确，但代码可读性差）—— 一定在 Rust 用 `u32::from_le_bytes`，Swift 用 `UInt32(littleEndian:)`
- **`SequenceGuard::NextOutbound` 与 `AcceptAndAdvance` 共用 `highest_`**：文档明确"一实例一方向"，`MixedInboundOutboundWarning` 测试就是提醒；code review 时要盯这一点
- **HMAC 计算范围**：**只覆盖 header + payload，不覆盖 hmac 字段本身**（那样是循环依赖）；`EncodeUnsigned` 就是为这个用途设计的
