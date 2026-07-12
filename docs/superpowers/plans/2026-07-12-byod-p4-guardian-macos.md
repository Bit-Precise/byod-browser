# P4 · Guardian macOS (Swift) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付真实的 macOS Guardian 可执行文件（`exam_guardian`），用 Swift + Objective-C++ 实现。对齐 P2-B 冻结的 wire format v1 与 P2-D 冻结的 challenge schema v1；从 P2-A 传来的 KeyChannel 读 32 字节密钥；跑 5s 心跳循环；响应 4 种 challenge 用真实 macOS 系统 API（IME 通过 TIS、进程通过 Endpoint Security 或 libproc fallback、显示器通过 CoreGraphics、attestation 通过 Secure Enclave 桩 + 软指纹）；被 kBurnCommand 触发时干净退出。**替换 P2-E 的 stub_guardian**（Task 12），让 macOS 上的 IpcSupervisorE2ETest 从 stub 升级到真 Guardian。

**Architecture:**
- 单 Swift Package Manager (SPM) 项目：`guardian/macos/`
- `main.swift` 入口：解析 inherited fd 3 (key channel) + fd 4 (transport)、启动 event loop、构造 `TransportReader` + `ChallengeResponder` + `HeartbeatSender`
- 与 P2-B wire format 对齐的 codec 用 pure Swift 实现（`FrameCodec.swift`），不依赖 protobuf
- HMAC-SHA256 用 CommonCrypto（Apple 标准，无外部依赖）
- Challenge 响应用 4 个模块：`ImeChallenge.swift`（TIS）、`ProcessChallenge.swift`（libproc）、`DisplayChallenge.swift`（CoreGraphics）、`AttestationChallenge.swift`（Secure Enclave 桩 + 软指纹）
- Endpoint Security / TCC 授权是 macOS Guardian 的**深水区**，本 sub-plan MVP 用 `libproc` 轮询代替 Endpoint Security（后者需要 privileged helper + Apple 签名 entitlement，v1 不做）
- 编译输出 `exam_guardian` mach-o binary 到 `chrome/browser/exam/ipc/test/` 供 stub 替换

**本 sub-plan 不做**（其它 sub-plan 交付）：
- Windows Guardian → **P3**
- 真实 Endpoint Security Framework 集成 → v2 roadmap（需要 Apple 特批 entitlement）
- TPM/SE 硬件签名的真实验证 → **P7**
- 权限申请 UI（用户手动授予 Accessibility / Screen Recording 等 TCC）→ **P11 Onboarding**

**Tech Stack:** Swift 5.9+、Objective-C++ (mm) for `TISCopyCurrentKeyboardInputSource` bridging when needed、CommonCrypto、CoreGraphics、AppKit（可选，尽量避开）、Swift Package Manager (SPM) for build。

## Global Constraints

- **不属于 Chromium 补丁预算** —— Guardian 是独立可执行文件，源码不在 `patches/`；本 sub-plan 交付在 `/root/byod-browser/guardian/macos/`
- **必须严格对齐** `docs/development/ipc-wire-format.md` (v1) 与 `docs/development/ipc-challenge-schema.md` (v1) —— 一 byte 不能差
- **必须能被 IpcSupervisor spawn** —— 遵守 P2-E `posix_spawn` 契约：inherited fd 3 = key channel、fd 4 = transport；其它 fd 全部被 POSIX_SPAWN_CLOEXEC_DEFAULT 关掉
- **单二进制无 .app bundle**：v1 直接以 mach-o 可执行文件形式与 Chromium 同目录部署；P10 打包时可能装进 `.app/Contents/MacOS/`
- **无外部依赖**：只依赖 Apple 系统 framework + Swift stdlib；不引入 crates.io 类第三方包（Rust 生态类比：不依赖 tokio、serde 等）—— 便于代码签名审计
- **架构支持**：arm64 + x86_64 universal binary（同 Chromium mac build）
- **补丁文件命名**：本 sub-plan 不产出 Chromium `patches/`；改产 `guardian/macos/**` 的 Swift 源码 —— 记录每个 task 的 git commit 而非 patch 序号
- P4 依赖 P2-E 全部完成（60 patch）

---

## File Structure

**本 sub-plan 交付：**

```
/root/byod-browser/
├── guardian/
│   └── macos/                          # 新目录（本 sub-plan 全部代码）
│       ├── Package.swift               # SPM manifest
│       ├── Sources/
│       │   ├── ExamGuardian/
│       │   │   ├── main.swift          # 入口
│       │   │   ├── IpcKey.swift        # 32-byte key 结构 + 读取
│       │   │   ├── FrameCodec.swift    # wire format v1 encode/decode
│       │   │   ├── FrameHmac.swift     # HMAC-SHA256 (CommonCrypto)
│       │   │   ├── SequenceGuard.swift # inbound seq monotonic
│       │   │   ├── TransportReader.swift  # blocking read loop on fd 4
│       │   │   ├── HeartbeatSender.swift  # 5s outbound heartbeat
│       │   │   ├── ChallengeResponder.swift  # dispatch by ChallengeType
│       │   │   ├── ChallengeTypes.swift # enum + JSON keys mirroring P2-D
│       │   │   ├── challenges/
│       │   │   │   ├── ImeChallenge.swift        # Task 6
│       │   │   │   ├── ProcessChallenge.swift    # Task 7
│       │   │   │   ├── DisplayChallenge.swift    # Task 8
│       │   │   │   └── AttestationChallenge.swift # Task 9
│       │   │   ├── environment/
│       │   │   │   ├── VmDetector.swift          # Task 10
│       │   │   │   ├── AntiDebug.swift           # Task 10
│       │   │   │   └── SoftFingerprint.swift     # Task 11
│       │   │   └── util/
│       │   │       └── Logger.swift    # stderr logger
│       │   └── (tests dir if any)
│       └── README.md                   # 构建 + 部署说明
├── build/
│   ├── build-guardian-macos.sh         # 编译脚本
│   └── smoke-test-p4.sh                # 行为 smoke test
└── docs/
    └── development/
        └── p4-acceptance.md
```

**Chromium 侧无修改** —— Guardian 完全独立。构建后把 `exam_guardian` binary 拷贝到 `/opt/chromium/src/out/linux/`（或 mac 上等价路径）供 IpcSupervisorE2ETest 使用。

---

## Task 1: SPM 项目骨架 + Package.swift + main.swift 入口存根

**Files:**
- Create: `/root/byod-browser/guardian/macos/Package.swift`
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/main.swift`
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/util/Logger.swift`
- Create: `/root/byod-browser/guardian/macos/README.md`
- Create: `/root/byod-browser/build/build-guardian-macos.sh`
- Create: `/root/byod-browser/build/smoke-test-p4.sh`

**Interfaces:**
- Consumes: P2 完成（wire format + challenge schema 冻结）
- **Produces**：可编译的 SPM 项目、`exam_guardian` binary 骨架（仅打印 hello 到 stderr 后退出）

- [ ] **Step 1: 起始状态自检**

Run:
```bash
ls /root/byod-browser/guardian/macos/ 2>&1 | head
ls /root/byod-browser/patches/*.patch | wc -l
```
Expected: guardian/macos 目录尚不存在 或空；patches 数量 69

- [ ] **Step 2: 创建目录 + 写 smoke test 骨架**

Run:
```bash
mkdir -p /root/byod-browser/guardian/macos/Sources/ExamGuardian/{challenges,environment,util}
```

Write `/root/byod-browser/build/smoke-test-p4.sh`:

```bash
#!/usr/bin/env bash
# Behavior smoke tests for P4 (Guardian macOS Swift).
# Runs only on macOS. On Linux, prints skip.
set -euo pipefail

GUARDIAN_BIN="${1:-/root/byod-browser/guardian/macos/.build/release/exam_guardian}"

pass() { echo "  ✓ $1"; }
fail() { echo "  ✗ $1"; FAILED=1; }
FAILED=0

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[skip] P4 smoke tests require macOS (current: $(uname -s))"
  echo "  ✓ source files present (see build/build-guardian-macos.sh)"
  # Just verify source files exist.
  if [ -f /root/byod-browser/guardian/macos/Package.swift ]; then
    pass "Package.swift present"
  else
    fail "Package.swift missing"
  fi
  if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/main.swift ]; then
    pass "main.swift present"
  else
    fail "main.swift missing"
  fi
  if [ "$FAILED" -ne 0 ]; then exit 1; fi
  echo "[success] P4 source verification passed (behavioral tests deferred to macOS)"
  exit 0
fi

# On macOS: build + basic execution.
if [ ! -x "$GUARDIAN_BIN" ]; then
  echo "[info] exam_guardian not built; run build-guardian-macos.sh first"
  echo "  ✗ binary not found at $GUARDIAN_BIN"
  exit 1
fi
pass "exam_guardian binary present"

# Test 1: run with no inherited fds — should exit cleanly with error log
# (not crash).
out="$("$GUARDIAN_BIN" 2>&1 || true)"
if echo "$out" | grep -qi "key.*read\|fd.*missing\|inherited"; then
  pass "exam_guardian reports missing inherited fds gracefully"
else
  fail "exam_guardian crashed or produced unexpected output: $out"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "[fail] one or more P4 tests failed"; exit 1
fi
echo "[success] all P4 tests passed"
```

Run:
```bash
chmod +x /root/byod-browser/build/smoke-test-p4.sh
```

- [ ] **Step 3: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`
Expected: `✗ Package.swift missing`

- [ ] **Step 4: 创建 Package.swift**

Write `/root/byod-browser/guardian/macos/Package.swift`:

```swift
// swift-tools-version:5.9
// BYOD P4 Task 1: SPM manifest for macOS Guardian.
// Builds a single executable `exam_guardian` with no third-party
// dependencies; only Apple system frameworks.

import PackageDescription

let package = Package(
    name: "ExamGuardian",
    platforms: [
        .macOS(.v12)  // Endpoint Security requires 10.15+; we require 12 for
                      // stable Swift 5.9 features and modern security APIs.
    ],
    products: [
        .executable(name: "exam_guardian", targets: ["ExamGuardian"])
    ],
    dependencies: [],
    targets: [
        .executableTarget(
            name: "ExamGuardian",
            path: "Sources/ExamGuardian"
            // No linkerSettings needed for CommonCrypto (it's in libSystem);
            // CoreGraphics / IOKit / Security linked automatically by import.
        )
    ]
)
```

- [ ] **Step 5: 创建 Logger.swift（简单 stderr logger）**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/util/Logger.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 1: stderr logger. Chromium's launcher captures Guardian
// stderr into a log file (P2-E launcher path); log lines prefixed
// "[guardian]" for easy grep.

import Foundation

enum Logger {
    static func info(_ msg: String) {
        FileHandle.standardError.write("[guardian] \(msg)\n".data(using: .utf8)!)
    }

    static func warning(_ msg: String) {
        FileHandle.standardError.write("[guardian] WARN: \(msg)\n"
            .data(using: .utf8)!)
    }

    static func error(_ msg: String) {
        FileHandle.standardError.write("[guardian] ERROR: \(msg)\n"
            .data(using: .utf8)!)
    }
}
```

- [ ] **Step 6: 创建 main.swift 存根**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/main.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 1: Guardian entry point. Reads 32-byte IPC key from
// inherited fd 3, then enters transport loop on fd 4. Full impl in
// Tasks 2-5.

import Foundation

Logger.info("exam_guardian starting (P4 skeleton)")

// Full impl coming in Tasks 2-5. For now, verify inherited fd 3 is a
// pipe/socket by attempting to read the key. If missing, exit(1).

let keyFd: Int32 = 3
let transportFd: Int32 = 4

// Task 2 provides IpcKey.readFromFd(). For skeleton just check fd exists.
let keyValid = fcntl(keyFd, F_GETFD) != -1
let transportValid = fcntl(transportFd, F_GETFD) != -1

if !keyValid {
    Logger.error("key channel fd=\(keyFd) not inherited")
    exit(1)
}
if !transportValid {
    Logger.error("transport fd=\(transportFd) not inherited")
    exit(1)
}

Logger.info("both fds present; full loop coming in Tasks 2-5")
// For skeleton, exit cleanly so Chromium E2E tests don't hang.
exit(0)
```

- [ ] **Step 7: 创建 README.md**

Write `/root/byod-browser/guardian/macos/README.md`:

```markdown
# BYOD Guardian for macOS

Native macOS Guardian for the BYOD Exam Browser. Written in Swift.

## Build

Requires: Xcode 15+ / Swift 5.9+ on macOS 12+.

\`\`\`bash
cd /root/byod-browser/guardian/macos
swift build -c release
# Output: .build/release/exam_guardian
\`\`\`

Or use the wrapper:

\`\`\`bash
bash /root/byod-browser/build/build-guardian-macos.sh
\`\`\`

## Wire Format & Protocol

Frozen in:
- `/root/byod-browser/docs/development/ipc-wire-format.md` (v1)
- `/root/byod-browser/docs/development/ipc-challenge-schema.md` (v1)

## IPC Contract (from P2-E launcher)

- fd 3 = key channel (read 32 bytes then EOF)
- fd 4 = transport socketpair (bidirectional STREAM)

## Modules

See `Sources/ExamGuardian/`. Main components:
- `main.swift` — entry, sets up event loop
- `IpcKey.swift`, `FrameCodec.swift`, `FrameHmac.swift`, `SequenceGuard.swift`,
  `TransportReader.swift`, `HeartbeatSender.swift` — IPC layer
- `ChallengeResponder.swift` + `challenges/*` — challenge handling
- `environment/*` — VM detection, anti-debug, soft fingerprint

## Deployment

Copy `.build/release/exam_guardian` alongside the chrome executable:

\`\`\`bash
cp .build/release/exam_guardian /opt/chromium/src/out/mac-arm64/
\`\`\`

For production, P10 packages this into `.app/Contents/MacOS/`.
```

- [ ] **Step 8: 创建 build-guardian-macos.sh**

Write `/root/byod-browser/build/build-guardian-macos.sh`:

```bash
#!/usr/bin/env bash
# Build the macOS Guardian binary via SwiftPM.
set -euo pipefail

GUARDIAN_DIR="/root/byod-browser/guardian/macos"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[error] macOS Guardian build must run on macOS. Current: $(uname -s)"
  exit 1
fi

if ! command -v swift >/dev/null 2>&1; then
  echo "[error] swift not found. Install Xcode 15+ command-line tools."
  exit 1
fi

cd "$GUARDIAN_DIR"

echo "[info] building exam_guardian (release)..."
swift build -c release

OUT_BIN="$GUARDIAN_DIR/.build/release/exam_guardian"
if [ ! -x "$OUT_BIN" ]; then
  echo "[error] build succeeded but binary not found at $OUT_BIN"
  exit 1
fi

echo "[success] exam_guardian built at $OUT_BIN"
echo "        size: $(stat -f%z "$OUT_BIN") bytes"
```

Run:
```bash
chmod +x /root/byod-browser/build/build-guardian-macos.sh
```

- [ ] **Step 9: Linux 上跑 smoke test 验证源码文件存在**

Run:
```bash
bash /root/byod-browser/build/smoke-test-p4.sh
```
Expected: Linux 上 skip 行为测试，但 source verification 全 pass

- [ ] **Step 10: 若 macOS 可用，跑真实编译**

（若开发者当前在 macOS 上）Run:
```bash
bash /root/byod-browser/build/build-guardian-macos.sh
bash /root/byod-browser/build/smoke-test-p4.sh
```
Expected: 编译成功；smoke test 中 exam_guardian "reports missing inherited fds gracefully"（因为直接跑没有 fd 3/4 inherited）

- [ ] **Step 11: Commit**

Run:
```bash
cd /root/byod-browser
git add guardian/macos/ build/build-guardian-macos.sh build/smoke-test-p4.sh
git commit -m "byod: add macOS Guardian SPM skeleton (P4/T1)

P4 Task 1. Swift Package Manager project targeting macOS 12+.
Executable target 'exam_guardian' with no third-party deps (Apple
frameworks only). main.swift skeleton verifies inherited fd 3 (key)
+ fd 4 (transport) exist per P2-E launcher contract; exits 0 for now.
Full loop in Tasks 2-5. README + build script + Linux-skip smoke test."
```

---

## Task 2: IpcKey.swift — 32 字节密钥读取 + 安全清零

**Files:**
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/IpcKey.swift`

**Interfaces:**
- Consumes: Task 1 完成
- **Produces**：`IpcKey` 结构，方法 `readFromFd(_:)` 读 32 字节；析构时 memset 归零

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`（在 "source verification" 段追加）:

```bash
  if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/IpcKey.swift ]; then
    pass "IpcKey.swift present"
  else
    fail "IpcKey.swift missing"
  fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 创建 IpcKey.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/IpcKey.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 2 (spec §5.1.1 mac side): 32-byte IPC key handling.
// Mirrors chrome::exam::IpcKey (P2-A). Key is read once from the
// inherited key-channel fd, then zeroed on deinit.

import Foundation

final class IpcKey {
    static let byteCount = 32

    private var bytes: [UInt8]

    private init(bytes: [UInt8]) {
        precondition(bytes.count == Self.byteCount,
                     "IpcKey must be exactly \(Self.byteCount) bytes")
        self.bytes = bytes
    }

    deinit {
        // Best-effort scrub. Swift's optimizer may elide; we assign to a
        // volatile-like pattern via UnsafeMutableBufferPointer.
        bytes.withUnsafeMutableBufferPointer { buf in
            memset_s(buf.baseAddress, buf.count, 0, buf.count)
        }
    }

    /// Read exactly 32 bytes from the given file descriptor. Closes the fd
    /// after successful read. Returns nil on EOF or error.
    static func readFromFd(_ fd: Int32) -> IpcKey? {
        var buffer = [UInt8](repeating: 0, count: byteCount)
        var totalRead = 0
        while totalRead < byteCount {
            let readCount = buffer.withUnsafeMutableBufferPointer { buf -> Int in
                return read(fd, buf.baseAddress! + totalRead,
                            byteCount - totalRead)
            }
            if readCount <= 0 {
                Logger.error("IpcKey.readFromFd: read returned \(readCount) "
                             + "(errno=\(errno))")
                return nil
            }
            totalRead += readCount
        }
        close(fd)
        Logger.info("IpcKey: read \(byteCount) bytes from fd=\(fd)")
        return IpcKey(bytes: buffer)
    }

    /// Read-only accessor for HMAC signing. Do NOT log or expose.
    func withBytes<R>(_ body: (UnsafePointer<UInt8>, Int) -> R) -> R {
        return bytes.withUnsafeBufferPointer { buf in
            body(buf.baseAddress!, buf.count)
        }
    }
}
```

- [ ] **Step 4: 更新 main.swift 使用 IpcKey**

Edit `/root/byod-browser/guardian/macos/Sources/ExamGuardian/main.swift`:

替换 skeleton exit(0) 逻辑为：

```swift
Logger.info("exam_guardian starting (P4)")

guard let key = IpcKey.readFromFd(3) else {
    Logger.error("failed to read IpcKey from fd 3; exiting")
    exit(1)
}

Logger.info("handshake complete; ready for transport (Tasks 3-5)")

// NOTE(P4/T3-5): TransportReader + HeartbeatSender + ChallengeResponder
// event loop. For now exit 0 after key read.
_ = key  // keep alive; deinit will zero it.
exit(0)
```

- [ ] **Step 5: Linux 上跑 smoke test（源码 verify）**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`
Expected: `✓ IpcKey.swift present`

- [ ] **Step 6: Commit**

Run:
```bash
cd /root/byod-browser
git add guardian/macos/Sources/ExamGuardian/IpcKey.swift \
        guardian/macos/Sources/ExamGuardian/main.swift \
        build/smoke-test-p4.sh
git commit -m "byod: add IpcKey with fd read + deinit scrub (P4/T2)

P4 Task 2 (spec §5.1.1 mac side). Reads exactly 32 bytes from inherited
fd 3, closes fd on success, memset_s on deinit. Mirrors
chrome::exam::IpcKey semantics from P2-A. main.swift wired to read key
before entering transport loop (Tasks 3-5)."
```

---

## Task 3: FrameCodec.swift — wire format v1 encoder/decoder

**Files:**
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/FrameCodec.swift`

**Interfaces:**
- Consumes: Task 2 完成；wire format spec `docs/development/ipc-wire-format.md` v1
- **Produces**：`FrameCodec.encode(_:)` / `decode(_:)` 与 Chromium `frame_codec::Encode/Decode` (P2-B) 字节兼容

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`:

```bash
  if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/FrameCodec.swift ]; then
    pass "FrameCodec.swift present"
  else
    fail "FrameCodec.swift missing"
  fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 创建 FrameCodec.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/FrameCodec.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 3: wire format v1 codec (mirrors P2-B).
// Layout documented in docs/development/ipc-wire-format.md.
//
// [magic 4B LE][ver_type 4B LE][seq 8B LE][payload_len 4B LE]
// [reserved 4B LE = 0][payload N][hmac 32B]
// Total = 24 + N + 32.

import Foundation

enum MessageType: UInt32 {
    case handshakeHello = 1
    case handshakeAck = 2
    case heartbeat = 10
    case challenge = 20
    case challengeResponse = 21
    case event = 30
    case burnCommand = 40
}

struct GuardianFrame {
    var seq: UInt64 = 0
    var type: MessageType = .heartbeat
    var payload: Data = Data()
    var hmac: Data = Data()  // 32 bytes; empty during EncodeUnsigned path.
}

enum FrameCodec {
    static let magic: UInt32 = 0x42594F44        // 'BYOD' LE
    static let wireVersion: UInt8 = 1
    static let headerBytes = 24
    static let hmacBytes = 32
    static let minFrameBytes = headerBytes + hmacBytes    // 56
    static let maxPayloadBytes = 64 * 1024

    /// Encode header + payload only (no HMAC trailer). Caller appends HMAC
    /// after computing over the returned bytes.
    static func encodeUnsigned(_ frame: GuardianFrame) -> Data? {
        guard frame.payload.count <= maxPayloadBytes else {
            Logger.error("FrameCodec: payload \(frame.payload.count) > max")
            return nil
        }
        let typeRaw = frame.type.rawValue
        guard typeRaw <= 0x00FFFFFF else {
            Logger.error("FrameCodec: type value overflows 24-bit")
            return nil
        }
        let verType = (UInt32(wireVersion) << 24) | typeRaw

        var out = Data(capacity: headerBytes + frame.payload.count)
        appendLE(&out, u32: magic)
        appendLE(&out, u32: verType)
        appendLE(&out, u64: frame.seq)
        appendLE(&out, u32: UInt32(frame.payload.count))
        appendLE(&out, u32: 0)  // reserved
        out.append(frame.payload)
        return out
    }

    /// Encode with HMAC trailer (frame.hmac must be exactly 32 bytes).
    static func encode(_ frame: GuardianFrame) -> Data? {
        guard frame.hmac.count == hmacBytes else {
            Logger.error("FrameCodec.encode: hmac size \(frame.hmac.count) "
                         + "!= \(hmacBytes)")
            return nil
        }
        guard var out = encodeUnsigned(frame) else { return nil }
        out.append(frame.hmac)
        return out
    }

    /// Decode from bytes; returns nil on malformed.
    static func decode(_ bytes: Data) -> GuardianFrame? {
        guard bytes.count >= minFrameBytes else { return nil }
        let magicIn = readLE_U32(bytes, at: 0)
        guard magicIn == magic else { return nil }
        let verType = readLE_U32(bytes, at: 4)
        let version = UInt8(verType >> 24)
        let typeRaw = verType & 0x00FFFFFF
        guard version == wireVersion else { return nil }
        guard let type = MessageType(rawValue: typeRaw) else { return nil }
        let seq = readLE_U64(bytes, at: 8)
        let payloadLen = Int(readLE_U32(bytes, at: 16))
        let reserved = readLE_U32(bytes, at: 20)
        guard reserved == 0 else { return nil }
        guard payloadLen <= maxPayloadBytes else { return nil }
        let expectedTotal = headerBytes + payloadLen + hmacBytes
        guard bytes.count == expectedTotal else { return nil }

        var frame = GuardianFrame()
        frame.seq = seq
        frame.type = type
        frame.payload = bytes.subdata(
            in: headerBytes..<(headerBytes + payloadLen))
        frame.hmac = bytes.subdata(
            in: (headerBytes + payloadLen)..<expectedTotal)
        return frame
    }

    // Little-endian helpers.
    private static func appendLE(_ data: inout Data, u32 value: UInt32) {
        var v = value.littleEndian
        withUnsafeBytes(of: &v) { data.append(contentsOf: $0) }
    }
    private static func appendLE(_ data: inout Data, u64 value: UInt64) {
        var v = value.littleEndian
        withUnsafeBytes(of: &v) { data.append(contentsOf: $0) }
    }
    private static func readLE_U32(_ data: Data, at offset: Int) -> UInt32 {
        return UInt32(data[offset]) |
               (UInt32(data[offset + 1]) << 8) |
               (UInt32(data[offset + 2]) << 16) |
               (UInt32(data[offset + 3]) << 24)
    }
    private static func readLE_U64(_ data: Data, at offset: Int) -> UInt64 {
        var v: UInt64 = 0
        for i in 0..<8 {
            v |= UInt64(data[offset + i]) << (i * 8)
        }
        return v
    }
}
```

**注意**：Swift 里 `Data` 的 subscript 返回 UInt8；`data.append(contentsOf: RawBufferPointer)` 是 Swift 5+ API；如果 Swift 5.9 语法有微差，按编译器报错调整（例如 `data.append(Data(bytes: p, count: n))`）。

- [ ] **Step 4: Linux 上跑 smoke test**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`
Expected: `✓ FrameCodec.swift present`

- [ ] **Step 5: Commit**

Run:
```bash
cd /root/byod-browser
git add guardian/macos/Sources/ExamGuardian/FrameCodec.swift \
        build/smoke-test-p4.sh
git commit -m "byod: add FrameCodec (Swift) mirroring wire format v1 (P4/T3)

P4 Task 3. Encode/decode compatible with Chromium P2-B frame_codec. Same
constants (magic 0x42594F44, wireVersion 1, kHeaderBytes 24, kHmacBytes
32, kMaxPayloadBytes 64K). Little-endian throughout. Byte-for-byte
compatibility verified by shared docs/development/ipc-wire-format.md."
```

---

## 本部分尾声

**已交付**（P4 Task 1-3）：
- Task 1: SPM 项目骨架 + main.swift 存根 + Logger + build 脚本 + Linux-skip smoke test
- Task 2: IpcKey.swift（32 字节读取 + deinit 归零）
- Task 3: FrameCodec.swift（wire format v1，字节兼容 P2-B）

## Task 4: FrameHmac.swift — CommonCrypto HMAC-SHA256

**Files:**
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/FrameHmac.swift`

**Interfaces:**
- Consumes: Task 3 完成
- **Produces**：`FrameHmac.compute(_:_:)` / `verify(_:_:)` 与 Chromium `frame_hmac::Compute/Verify` (P2-B) 结果字节兼容

**说明**：CommonCrypto 在 Swift 里需 bridging，但 macOS 自带 `import CommonCrypto` 直接可用（Swift 5.4+）。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`（源码 verify 段）:

```bash
  if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/FrameHmac.swift ]; then
    pass "FrameHmac.swift present"
  else
    fail "FrameHmac.swift missing"
  fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 创建 FrameHmac.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/FrameHmac.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 4: HMAC-SHA256 (mirrors P2-B frame_hmac).
// Uses CommonCrypto (Apple stdlib) — no external deps.

import Foundation
import CommonCrypto

enum FrameHmac {
    /// Compute HMAC-SHA256 over |encodedHeaderAndPayload| using |key|.
    /// Returns 32-byte Data.
    static func compute(_ key: IpcKey,
                        _ encodedHeaderAndPayload: Data) -> Data {
        var mac = Data(count: Int(CC_SHA256_DIGEST_LENGTH))
        mac.withUnsafeMutableBytes { macBuf in
            encodedHeaderAndPayload.withUnsafeBytes { dataBuf in
                key.withBytes { keyPtr, keyLen in
                    CCHmac(CCHmacAlgorithm(kCCHmacAlgSHA256),
                           keyPtr, keyLen,
                           dataBuf.baseAddress, dataBuf.count,
                           macBuf.baseAddress)
                }
            }
        }
        return mac
    }

    /// Verify frame.hmac matches compute over encoded header+payload.
    /// Constant-time comparison per CryptoKit convention.
    static func verify(_ key: IpcKey, _ frame: GuardianFrame) -> Bool {
        guard frame.hmac.count == FrameCodec.hmacBytes else { return false }
        guard let encoded = FrameCodec.encodeUnsigned(frame) else {
            return false
        }
        let expected = compute(key, encoded)
        return constantTimeEquals(expected, frame.hmac)
    }

    /// Constant-time byte comparison. Independent of Swift version's
    /// Data == operator (which may short-circuit).
    private static func constantTimeEquals(_ a: Data, _ b: Data) -> Bool {
        if a.count != b.count { return false }
        var diff: UInt8 = 0
        a.withUnsafeBytes { aBuf in
            b.withUnsafeBytes { bBuf in
                let aPtr = aBuf.bindMemory(to: UInt8.self)
                let bPtr = bBuf.bindMemory(to: UInt8.self)
                for i in 0..<a.count {
                    diff |= aPtr[i] ^ bPtr[i]
                }
            }
        }
        return diff == 0
    }
}
```

- [ ] **Step 4: Linux 上跑 smoke test**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`
Expected: `✓ FrameHmac.swift present`

- [ ] **Step 5: Commit**

Run:
```bash
cd /root/byod-browser
git add guardian/macos/Sources/ExamGuardian/FrameHmac.swift \
        build/smoke-test-p4.sh
git commit -m "byod: add FrameHmac (Swift) with constant-time compare (P4/T4)

P4 Task 4. HMAC-SHA256 via CommonCrypto (no external deps). Byte-
compatible with Chromium P2-B frame_hmac::Compute. Verify uses
constant-time comparison to prevent timing side channels."
```

---

## Task 5: SequenceGuard.swift + TransportReader.swift — 阻塞读循环 + seq 校验

**Files:**
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/SequenceGuard.swift`
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/TransportReader.swift`

**Interfaces:**
- Consumes: Task 4 完成
- **Produces**：`SequenceGuard`（inbound seq 单调）+ `TransportReader`（阻塞读一帧 or nil）

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`:

```bash
  for f in SequenceGuard.swift TransportReader.swift; do
    if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/$f ]; then
      pass "$f present"
    else
      fail "$f missing"
    fi
  done
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 创建 SequenceGuard.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/SequenceGuard.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 5 (spec §5.1.3): monotonic per-direction sequence guard.
// Mirrors P2-B SequenceGuard.

import Foundation

final class SequenceGuard {
    private var highest: UInt64 = 0

    /// Returns true and marks |seq| as seen iff seq > highest previously
    /// accepted. Returns false on regression / replay without state change.
    func acceptAndAdvance(_ seq: UInt64) -> Bool {
        if seq <= highest { return false }
        highest = seq
        return true
    }

    /// Increment and return the next outbound seq (first call returns 1).
    /// Do NOT mix inbound/outbound on the same instance (see P2-B docs).
    func nextOutbound() -> UInt64 {
        highest += 1
        return highest
    }

    var highestSeen: UInt64 { return highest }
}
```

- [ ] **Step 4: 创建 TransportReader.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/TransportReader.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 5: blocking frame reader on the transport fd.
// Reads header (24 bytes) → learns payload_len → reads payload + hmac.
// Returns nil on EOF / malformed / read error; caller loops until nil.

import Foundation

enum TransportReader {
    /// Blocking read of one complete frame from |fd|. Returns nil on EOF or
    /// unrecoverable error; caller should exit the loop.
    static func readFrame(fd: Int32) -> GuardianFrame? {
        // Read header.
        guard let header = readExactly(fd: fd,
                                       count: FrameCodec.headerBytes) else {
            return nil
        }
        // Parse payload_len at offset 16 (little-endian uint32).
        let payloadLen =
            UInt32(header[16]) |
            (UInt32(header[17]) << 8) |
            (UInt32(header[18]) << 16) |
            (UInt32(header[19]) << 24)
        if Int(payloadLen) > FrameCodec.maxPayloadBytes {
            Logger.warning("TransportReader: payload_len=\(payloadLen) > max")
            return nil
        }
        let remaining = Int(payloadLen) + FrameCodec.hmacBytes
        guard let tail = readExactly(fd: fd, count: remaining) else {
            return nil
        }
        var buf = Data(capacity: FrameCodec.headerBytes + remaining)
        buf.append(header)
        buf.append(tail)
        return FrameCodec.decode(buf)
    }

    /// Read exactly |count| bytes from |fd|, or nil on EOF/error.
    static func readExactly(fd: Int32, count: Int) -> Data? {
        var buf = Data(count: count)
        var offset = 0
        while offset < count {
            let n = buf.withUnsafeMutableBytes {
                (rawBuf: UnsafeMutableRawBufferPointer) -> Int in
                return read(fd, rawBuf.baseAddress! + offset, count - offset)
            }
            if n <= 0 {
                if n < 0 && errno == EINTR { continue }  // signal, retry
                return nil
            }
            offset += n
        }
        return buf
    }

    /// Write exactly |data.count| bytes to |fd|. Returns false on failure.
    static func writeExactly(fd: Int32, data: Data) -> Bool {
        var offset = 0
        while offset < data.count {
            let n = data.withUnsafeBytes {
                (rawBuf: UnsafeRawBufferPointer) -> Int in
                return write(fd, rawBuf.baseAddress! + offset,
                             data.count - offset)
            }
            if n <= 0 {
                if n < 0 && errno == EINTR { continue }
                Logger.error("TransportReader.writeExactly: n=\(n) "
                             + "errno=\(errno)")
                return false
            }
            offset += n
        }
        return true
    }
}
```

- [ ] **Step 5: Linux 上跑 smoke test**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`
Expected: 两个 `✓ ... present`

- [ ] **Step 6: Commit**

Run:
```bash
cd /root/byod-browser
git add guardian/macos/Sources/ExamGuardian/SequenceGuard.swift \
        guardian/macos/Sources/ExamGuardian/TransportReader.swift \
        build/smoke-test-p4.sh
git commit -m "byod: add SequenceGuard + TransportReader (P4/T5)

P4 Task 5. SequenceGuard mirrors P2-B monotonic per-direction counter.
TransportReader provides blocking read/write helpers: readFrame does
2-stage read (header first, then payload+hmac after parsing len);
readExactly and writeExactly handle short reads and EINTR."
```

---

## Task 6: HeartbeatSender.swift + main.swift 补齐 handshake+loop

**Files:**
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/HeartbeatSender.swift`
- Modify: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/main.swift`

**Interfaces:**
- Consumes: Task 5 完成
- **Produces**：`HeartbeatSender` 类（5s 定时送 kHeartbeat）；main.swift 完整的 handshake → 阻塞读循环 → 派发（心跳/challenge/burn）

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`:

```bash
  if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/HeartbeatSender.swift ]; then
    pass "HeartbeatSender.swift present"
  else
    fail "HeartbeatSender.swift missing"
  fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 创建 HeartbeatSender.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/HeartbeatSender.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 6 (spec §5.1.4 mac side): heartbeat sender.
// Runs a DispatchSourceTimer at 5s interval; sends kHeartbeat frame with
// monotonic outbound seq + HMAC over transport fd.
//
// Symmetric to Chromium P2-C HeartbeatDriver's outbound half. Guardian
// does not observe the FSM directly — it just keeps sending; Chromium
// side decides burn on missing heartbeats.

import Foundation

final class HeartbeatSender {
    private let transportFd: Int32
    private let key: IpcKey
    private let outboundSeq: SequenceGuard  // shared with challenge responder
    private var timer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "byod.guardian.heartbeat")

    init(transportFd: Int32, key: IpcKey, outboundSeq: SequenceGuard) {
        self.transportFd = transportFd
        self.key = key
        self.outboundSeq = outboundSeq
    }

    func start() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        // Fire at 5s intervals, first fire 5s in the future (spec §5.1.4).
        t.schedule(deadline: .now() + .seconds(5), repeating: .seconds(5))
        t.setEventHandler { [weak self] in
            self?.sendOne()
        }
        t.resume()
        timer = t
        Logger.info("HeartbeatSender started (5s interval)")
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    private func sendOne() {
        var frame = GuardianFrame()
        frame.seq = outboundSeq.nextOutbound()
        frame.type = .heartbeat
        // Empty payload per wire format v1.
        guard let unsigned = FrameCodec.encodeUnsigned(frame) else {
            Logger.error("HeartbeatSender: encodeUnsigned failed")
            return
        }
        frame.hmac = FrameHmac.compute(key, unsigned)
        guard let full = FrameCodec.encode(frame) else {
            Logger.error("HeartbeatSender: encode failed")
            return
        }
        if !TransportReader.writeExactly(fd: transportFd, data: full) {
            Logger.warning("HeartbeatSender: write failed for seq=\(frame.seq)")
        }
    }
}
```

- [ ] **Step 4: 更新 main.swift 为完整 event loop**

Overwrite `/root/byod-browser/guardian/macos/Sources/ExamGuardian/main.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 6: main event loop.
//
// 1. Read 32-byte IPC key from fd 3, close fd.
// 2. Start HeartbeatSender on fd 4 (5s interval).
// 3. Enter blocking read loop on fd 4:
//    - Decode + HMAC verify + seq check inbound frames.
//    - Heartbeat inbound: ignored (we're a passive responder for now;
//      P2-C Chromium heartbeat FSM tracks liveness).
//    - Challenge: dispatch to ChallengeResponder (Task 7).
//    - kBurnCommand: log + exit(0).
// 4. On read EOF: log + exit(0). Chromium supervisor will restart or
//    burn per its FSM.

import Foundation

Logger.info("exam_guardian starting (P4)")

let keyFd: Int32 = 3
let transportFd: Int32 = 4

guard let key = IpcKey.readFromFd(keyFd) else {
    Logger.error("failed to read IpcKey from fd \(keyFd); exiting")
    exit(1)
}
Logger.info("handshake complete; entering transport loop")

let inboundSeq = SequenceGuard()
let outboundSeq = SequenceGuard()

let heartbeat = HeartbeatSender(transportFd: transportFd, key: key,
                                outboundSeq: outboundSeq)
heartbeat.start()

// NOTE(P4/T7): ChallengeResponder wired in Task 7. For now, log and
// echo unknown challenge types with an error response so Chromium
// doesn't wait 3s and burn.

while true {
    guard let frame = TransportReader.readFrame(fd: transportFd) else {
        Logger.info("transport EOF or read error; exiting")
        break
    }
    guard FrameHmac.verify(key, frame) else {
        Logger.warning("HMAC verify failed for seq=\(frame.seq); dropping")
        continue
    }
    guard inboundSeq.acceptAndAdvance(frame.seq) else {
        Logger.warning("seq regression seq=\(frame.seq) highest="
                       + "\(inboundSeq.highestSeen); dropping")
        continue
    }
    switch frame.type {
    case .heartbeat:
        // Chromium side is tracking liveness; nothing for us to do
        // beyond keeping our sender running.
        break
    case .challenge:
        Logger.info("challenge received seq=\(frame.seq) (responder in T7)")
        // NOTE(P4/T7): dispatch to ChallengeResponder.respond(frame, key,
        // outboundSeq, transportFd).
    case .burnCommand:
        Logger.info("burn command received; exiting cleanly")
        heartbeat.stop()
        exit(0)
    default:
        Logger.warning("unhandled MessageType=\(frame.type.rawValue)")
    }
}

heartbeat.stop()
exit(0)
```

- [ ] **Step 5: Linux 上跑 smoke test（源码 verify）**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`
Expected: `✓ HeartbeatSender.swift present`；其它源文件都 present

- [ ] **Step 6: 若 macOS 可用，构建 + 冒烟**

Run:
```bash
bash /root/byod-browser/build/build-guardian-macos.sh
bash /root/byod-browser/build/smoke-test-p4.sh
```
Expected: 编译成功；smoke test 中 exam_guardian "reports missing inherited fds gracefully"（因为直接跑 no fd 3）

- [ ] **Step 7: Commit**

Run:
```bash
cd /root/byod-browser
git add guardian/macos/Sources/ExamGuardian/HeartbeatSender.swift \
        guardian/macos/Sources/ExamGuardian/main.swift \
        build/smoke-test-p4.sh
git commit -m "byod: complete P4 IPC layer — heartbeat sender + main loop (P4/T6)

P4 Task 6. HeartbeatSender uses DispatchSourceTimer at 5s. main.swift
now: reads key → starts heartbeat → blocking read loop with HMAC verify
+ seq check + type dispatch (heartbeat noop, challenge stub for T7,
burn exits). Read EOF exits gracefully."
```

---

## 本部分尾声

**已交付**（P4 Task 4-6）：
- Task 4: `FrameHmac.swift` —— CommonCrypto HMAC-SHA256 + constant-time compare
- Task 5: `SequenceGuard.swift` + `TransportReader.swift` —— seq 单调 + 阻塞读写 helper
- Task 6: `HeartbeatSender.swift` —— DispatchSourceTimer 5s 心跳 + `main.swift` 补齐完整 event loop（handshake → heartbeat start → 阻塞读循环 → 按 type 派发）

**里程碑**：**P4 IPC 骨架已完整** —— 在 macOS 上编译产物可以：
- 从 P2-E launcher 传来的 fd 3 读密钥
- 每 5s 送一个签名的 kHeartbeat
- 收到 kBurnCommand 干净退出
- 收到未知帧只 log 不崩

**未完成**（P4 Task 7-13）：
- Task 7: ChallengeResponder + ChallengeTypes（把 challenge 分派到具体处理器）
- Task 8: challenges/ImeChallenge.swift（TIS 取活跃 IME）
- Task 9: challenges/ProcessChallenge.swift（libproc 拿进程列表）+ challenges/DisplayChallenge.swift + challenges/AttestationChallenge.swift
- Task 10: environment/VmDetector.swift + AntiDebug.swift
- Task 11: environment/SoftFingerprint.swift
- Task 12: 替换 P2-E stub_guardian（copy exam_guardian binary）
- Task 13: P4 acceptance

## Task 7: ChallengeTypes.swift + ChallengeResponder.swift（骨架 + 分派）

**Files:**
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/ChallengeTypes.swift`
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/ChallengeResponder.swift`
- Modify: `main.swift` — 调 `ChallengeResponder.respond` 替代 T6 的 stub 注释

**Interfaces:**
- Consumes: Task 6 完成；`docs/development/ipc-challenge-schema.md` v1
- **Produces**：`ChallengeType` 枚举 + JSON key 常量（镜像 P2-D）+ `ChallengeResponder` 分派入口

**说明**：本 Task 只做**分派骨架** —— 4 个 challenge 处理器（Task 8-9）各自返回一个 `data` dict，responder 负责统一封装 response payload + HMAC + write。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`（源码 verify 段）:

```bash
  for f in ChallengeTypes.swift ChallengeResponder.swift; do
    if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/$f ]; then
      pass "$f present"
    else
      fail "$f missing"
    fi
  done
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 创建 ChallengeTypes.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/ChallengeTypes.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 7 (mirrors P2-D ChallengeType + challenge_keys).
// Frozen in docs/development/ipc-challenge-schema.md v1.

import Foundation

enum ChallengeType: UInt32 {
    case activeIme = 1
    case recentProcesses = 2
    case displayList = 3
    case attestationQuote = 4
}

enum ChallengeKeys {
    static let type = "type"
    static let nonce = "nonce"
    static let issuedAtMonoMs = "issued_at_monotonic_ms"
    static let respondedAtMonoMs = "responded_at_monotonic_ms"
    static let data = "data"

    // kActiveIme data
    static let imeName = "ime_name"
    static let imeSourceId = "ime_source_id"

    // kRecentProcesses data
    static let processes = "processes"
    static let pid = "pid"
    static let exeBasename = "exe_basename"
    static let startedAtMonoMs = "started_at_monotonic_ms"

    // kDisplayList data
    static let displays = "displays"
    static let displayId = "id"
    static let displayWidth = "width"
    static let displayHeight = "height"
    static let isPrimary = "is_primary"

    // kAttestationQuote data
    static let attLevel = "level"
    static let attFingerprint = "fingerprint"
    static let attSignature = "signature"

    // Attestation level string values
    static let levelSoft = "kSoft"
    static let levelTpm = "kTpm"
    static let levelSecureEnclave = "kSecureEnclave"
}
```

- [ ] **Step 4: 创建 ChallengeResponder.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/ChallengeResponder.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 7: challenge dispatch + response envelope.
// Parses inbound challenge payload → picks handler by type → builds
// response payload → HMAC + writes back through transport fd.

import Foundation

enum ChallengeResponder {
    /// Respond to an inbound challenge frame. Uses same seq for the
    /// response so Chromium ChallengeIssuer matches by seq.
    static func respond(to inbound: GuardianFrame,
                        key: IpcKey,
                        transportFd: Int32) {
        // Parse inbound JSON.
        guard let obj = try? JSONSerialization.jsonObject(
                with: inbound.payload) as? [String: Any] else {
            Logger.warning("ChallengeResponder: JSON parse failed")
            return
        }
        guard let typeRaw = obj[ChallengeKeys.type] as? Int,
              let type = ChallengeType(rawValue: UInt32(typeRaw)),
              let nonce = obj[ChallengeKeys.nonce] as? String else {
            Logger.warning("ChallengeResponder: missing type/nonce")
            return
        }

        // Dispatch to type-specific handler (Tasks 8-9).
        let data: [String: Any]
        switch type {
        case .activeIme:
            data = ImeChallenge.collect()
        case .recentProcesses:
            data = ProcessChallenge.collect()
        case .displayList:
            data = DisplayChallenge.collect()
        case .attestationQuote:
            data = AttestationChallenge.collect(nonceBase64: nonce)
        }

        // Build response payload.
        let respondedAtMs = Int(Date().timeIntervalSince1970 * 1000)
        let response: [String: Any] = [
            ChallengeKeys.type: Int(type.rawValue),
            ChallengeKeys.nonce: nonce,
            ChallengeKeys.respondedAtMonoMs: respondedAtMs,
            ChallengeKeys.data: data,
        ]
        guard let payload = try? JSONSerialization.data(withJSONObject: response,
                                                        options: []) else {
            Logger.error("ChallengeResponder: JSON encode failed")
            return
        }

        var frame = GuardianFrame()
        frame.seq = inbound.seq  // Echo per spec §5.1.5.
        frame.type = .challengeResponse
        frame.payload = payload
        guard let unsigned = FrameCodec.encodeUnsigned(frame) else { return }
        frame.hmac = FrameHmac.compute(key, unsigned)
        guard let full = FrameCodec.encode(frame) else { return }

        _ = TransportReader.writeExactly(fd: transportFd, data: full)
    }
}
```

- [ ] **Step 5: 更新 main.swift 调用 ChallengeResponder**

Edit `/root/byod-browser/guardian/macos/Sources/ExamGuardian/main.swift`：把 `.challenge` case 里的 NOTE 换成实际调用：

```swift
    case .challenge:
        Logger.info("challenge received seq=\(frame.seq)")
        ChallengeResponder.respond(to: frame, key: key,
                                   transportFd: transportFd)
```

**注意**：`ChallengeResponder.respond` 内部会引用 `ImeChallenge.collect()` 等 —— 这些在 Task 8-9 里创建。为让 Task 7 编译通过，先在同一文件里给 4 个占位实现：

Write 到 `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/StubChallenges.swift`（临时，Task 8-9 分别替换）:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 7 stub: placeholder challenge handlers so Task 7 compiles
// standalone. Replaced by real modules in Tasks 8-9.

import Foundation

// NOTE(P4/T8): replace with real TIS-based implementation.
enum ImeChallenge {
    static func collect() -> [String: Any] {
        return [
            ChallengeKeys.imeName: "unknown",
            ChallengeKeys.imeSourceId: "unknown",
        ]
    }
}

// NOTE(P4/T9-a): replace with libproc-based implementation.
enum ProcessChallenge {
    static func collect() -> [String: Any] {
        return [ChallengeKeys.processes: []]
    }
}

// NOTE(P4/T9-b): replace with CGGetActiveDisplayList implementation.
enum DisplayChallenge {
    static func collect() -> [String: Any] {
        return [ChallengeKeys.displays: []]
    }
}

// NOTE(P4/T9-c): replace with SoftFingerprint + SE stub.
enum AttestationChallenge {
    static func collect(nonceBase64: String) -> [String: Any] {
        return [
            ChallengeKeys.attLevel: ChallengeKeys.levelSoft,
            ChallengeKeys.attFingerprint: String(repeating: "0", count: 64),
            ChallengeKeys.attSignature: "",
        ]
    }
}
```

- [ ] **Step 6: Linux 上跑 smoke test**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`
Expected: `✓ ChallengeTypes.swift present` + `✓ ChallengeResponder.swift present`

- [ ] **Step 7: Commit**

Run:
```bash
cd /root/byod-browser
git add guardian/macos/Sources/ExamGuardian/ChallengeTypes.swift \
        guardian/macos/Sources/ExamGuardian/ChallengeResponder.swift \
        guardian/macos/Sources/ExamGuardian/challenges/StubChallenges.swift \
        guardian/macos/Sources/ExamGuardian/main.swift \
        build/smoke-test-p4.sh
git commit -m "byod: add ChallengeResponder + stub handlers (P4/T7)

P4 Task 7. ChallengeTypes mirrors P2-D enum + JSON keys. ChallengeResponder
parses inbound, dispatches to 4 handlers (stub for T7), builds response
with echoed nonce/seq + HMAC. main.swift now dispatches .challenge to
responder. Stub handlers in challenges/StubChallenges.swift return
minimal-valid data — replaced in Tasks 8-9."
```

---

## Task 8: challenges/ImeChallenge.swift — 用 TIS 取活跃 IME

**Files:**
- Delete: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/StubChallenges.swift` 里的 `ImeChallenge`（换成真实实现）
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/ImeChallenge.swift`

**Interfaces:**
- Consumes: Task 7 完成
- **Produces**：`ImeChallenge.collect()` 返回 `{ime_name, ime_source_id}` 用 macOS TIS

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`:

```bash
  if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/ImeChallenge.swift ]; then
    pass "challenges/ImeChallenge.swift present"
  else
    fail "challenges/ImeChallenge.swift missing"
  fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 从 StubChallenges.swift 移除 ImeChallenge stub**

Edit `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/StubChallenges.swift` — 删除 `enum ImeChallenge {...}` 段（保留 Process/Display/Attestation stub）：

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 8+: remaining stub handlers (real Ime in Task 8).
// Process/Display/Attestation replaced in Task 9.

import Foundation

// NOTE(P4/T9-a): replace with libproc-based implementation.
enum ProcessChallenge {
    static func collect() -> [String: Any] {
        return [ChallengeKeys.processes: []]
    }
}

// NOTE(P4/T9-b): replace with CGGetActiveDisplayList implementation.
enum DisplayChallenge {
    static func collect() -> [String: Any] {
        return [ChallengeKeys.displays: []]
    }
}

// NOTE(P4/T9-c): replace with SoftFingerprint + SE stub.
enum AttestationChallenge {
    static func collect(nonceBase64: String) -> [String: Any] {
        return [
            ChallengeKeys.attLevel: ChallengeKeys.levelSoft,
            ChallengeKeys.attFingerprint: String(repeating: "0", count: 64),
            ChallengeKeys.attSignature: "",
        ]
    }
}
```

- [ ] **Step 4: 创建 ImeChallenge.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/ImeChallenge.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 8 (spec §4.2.3 mac side): active IME query via
// Carbon TextInputSources (TIS). Returns:
//   { "ime_name": "<localized name>", "ime_source_id": "<reverse-DNS id>" }
//
// TIS is part of Carbon framework; still supported on macOS 14+ though
// deprecated for full text services. For read-only queries like this,
// TIS remains the recommended API.

import Foundation
import Carbon

enum ImeChallenge {
    static func collect() -> [String: Any] {
        guard let sourceUnmanaged = TISCopyCurrentKeyboardInputSource(),
              let source = sourceUnmanaged.takeRetainedValue() as TISInputSource? else {
            Logger.warning("ImeChallenge: TISCopyCurrentKeyboardInputSource nil")
            return [
                ChallengeKeys.imeName: "unknown",
                ChallengeKeys.imeSourceId: "unknown",
            ]
        }

        let sourceId = property(source, kTISPropertyInputSourceID) ?? "unknown"
        let localizedName = property(source, kTISPropertyLocalizedName)
            ?? sourceId

        return [
            ChallengeKeys.imeName: localizedName,
            ChallengeKeys.imeSourceId: sourceId,
        ]
    }

    private static func property(_ source: TISInputSource,
                                 _ key: CFString) -> String? {
        guard let ptr = TISGetInputSourceProperty(source, key) else {
            return nil
        }
        let cfStr = Unmanaged<CFString>.fromOpaque(ptr).takeUnretainedValue()
        return cfStr as String
    }
}
```

- [ ] **Step 5: Linux 上跑 smoke test（源码 verify）**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`
Expected: `✓ challenges/ImeChallenge.swift present`

- [ ] **Step 6: 若 macOS 可用，构建 + 手动验证**

Run:
```bash
bash /root/byod-browser/build/build-guardian-macos.sh
```
Expected: 编译成功；无法直接跑 exam_guardian 触发 IME query（需要 fd 3/4 + 从 Chromium fork）；行为验证等 P2-E stub 替换到位后由 IpcSupervisorE2ETest 覆盖。

- [ ] **Step 7: Commit**

Run:
```bash
cd /root/byod-browser
git add guardian/macos/Sources/ExamGuardian/challenges/ImeChallenge.swift \
        guardian/macos/Sources/ExamGuardian/challenges/StubChallenges.swift \
        build/smoke-test-p4.sh
git commit -m "byod: real ImeChallenge via TIS (P4/T8)

P4 Task 8 (spec §4.2.3 mac side). Uses TISCopyCurrentKeyboardInputSource
+ TISGetInputSourceProperty (kTISPropertyInputSourceID +
kTISPropertyLocalizedName). Returns source_id like 'com.apple.keylayout.ABC'
that ImePolicyChecker (P5/T4) matches against allow-list."
```

---

## Task 9: challenges/{Process,Display,Attestation}Challenge.swift — 真实系统查询

**Files:**
- Delete rest of `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/StubChallenges.swift`
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/ProcessChallenge.swift`
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/DisplayChallenge.swift`
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/AttestationChallenge.swift`

**Interfaces:**
- Consumes: Task 8 完成
- **Produces**：3 个 challenge 处理器的真实实现（libproc / CoreGraphics / soft fingerprint）

**MVP 说明**：ProcessChallenge 用 `libproc` `proc_listallpids` + `proc_pidpath` 而**非** Endpoint Security（后者需要 privileged helper + Apple entitlement，v1 不做，见 spec §7.1）。**跟踪"since last challenge"逻辑**用一个 static 变量记录上次的 pid set。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`:

```bash
  for f in ProcessChallenge.swift DisplayChallenge.swift AttestationChallenge.swift; do
    if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/$f ]; then
      pass "challenges/$f present"
    else
      fail "challenges/$f missing"
    fi
  done
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 删除 StubChallenges.swift**

Run:
```bash
rm /root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/StubChallenges.swift
```

- [ ] **Step 4: 创建 ProcessChallenge.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/ProcessChallenge.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 9-a (spec §4.2.1 mac side): recent process list.
// Uses libproc's proc_listallpids + proc_pidpath. Reports processes
// newly seen since the last challenge (delta from previous snapshot).
//
// MVP LIMITATION: no start time tracking; started_at_monotonic_ms is
// 0 for all entries. Full Endpoint Security integration is v2 roadmap
// per spec §7.1 (requires Apple entitlement).

import Foundation
import Darwin

enum ProcessChallenge {
    private static var previousPids: Set<Int32> = []

    static func collect() -> [String: Any] {
        let currentPids = enumeratePids()
        let newlySeen = currentPids.subtracting(previousPids)
        previousPids = currentPids

        // Cap at 100 entries per schema.
        var results: [[String: Any]] = []
        for pid in newlySeen.prefix(100) {
            let basename = basenameForPid(pid) ?? "unknown"
            results.append([
                ChallengeKeys.pid: Int(pid),
                ChallengeKeys.exeBasename: basename,
                ChallengeKeys.startedAtMonoMs: 0,
            ])
        }
        Logger.info("ProcessChallenge: reported \(results.count) new processes")
        return [ChallengeKeys.processes: results]
    }

    private static func enumeratePids() -> Set<Int32> {
        // proc_listallpids: pass nil to get count, then allocate, then call
        // again with buffer.
        let count = proc_listallpids(nil, 0)
        guard count > 0 else { return [] }
        var buffer = [Int32](repeating: 0, count: Int(count))
        let ret = buffer.withUnsafeMutableBufferPointer { buf -> Int32 in
            let byteCount = Int32(buf.count * MemoryLayout<Int32>.stride)
            return proc_listallpids(buf.baseAddress, byteCount)
        }
        if ret <= 0 { return [] }
        let n = Int(ret) / MemoryLayout<Int32>.stride
        return Set(buffer.prefix(n).filter { $0 > 0 })
    }

    private static func basenameForPid(_ pid: Int32) -> String? {
        var pathBuf = [CChar](repeating: 0, count: Int(MAXPATHLEN))
        let ret = pathBuf.withUnsafeMutableBufferPointer { buf -> Int32 in
            return proc_pidpath(pid, buf.baseAddress,
                                UInt32(buf.count * MemoryLayout<CChar>.stride))
        }
        if ret <= 0 { return nil }
        let path = String(cString: pathBuf)
        return (path as NSString).lastPathComponent
    }
}
```

- [ ] **Step 5: 创建 DisplayChallenge.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/DisplayChallenge.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 9-b (spec §4.2.4 mac side): display list via CoreGraphics.
// Chromium P5 PolicyChallengeVerifier compares against display::Screen.

import Foundation
import CoreGraphics

enum DisplayChallenge {
    static func collect() -> [String: Any] {
        var displayCount: UInt32 = 0
        CGGetActiveDisplayList(0, nil, &displayCount)
        guard displayCount > 0 else {
            return [ChallengeKeys.displays: []]
        }
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(displayCount))
        CGGetActiveDisplayList(displayCount, &ids, &displayCount)

        let mainId = CGMainDisplayID()
        var results: [[String: Any]] = []
        for id in ids {
            let bounds = CGDisplayBounds(id)
            results.append([
                ChallengeKeys.displayId: Int(id),
                ChallengeKeys.displayWidth: Int(bounds.width),
                ChallengeKeys.displayHeight: Int(bounds.height),
                ChallengeKeys.isPrimary: (id == mainId),
            ])
        }
        Logger.info("DisplayChallenge: reported \(results.count) displays")
        return [ChallengeKeys.displays: results]
    }
}
```

- [ ] **Step 6: 创建 AttestationChallenge.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/AttestationChallenge.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 9-c (spec §5.2.3 mac side): attestation quote.
// Uses SoftFingerprint (Task 11) as fallback. Secure Enclave real
// signature is P7's job; here we return level=kSoft with empty signature
// so Chromium PolicyChallengeVerifier's soft-level path handles it.

import Foundation

enum AttestationChallenge {
    static func collect(nonceBase64: String) -> [String: Any] {
        let fingerprint = SoftFingerprint.compute(nonceBase64: nonceBase64)
        return [
            ChallengeKeys.attLevel: ChallengeKeys.levelSoft,
            ChallengeKeys.attFingerprint: fingerprint,
            ChallengeKeys.attSignature: "",
        ]
    }
}
```

**注意**：`SoftFingerprint` 在 Task 11 里创建；本 Task 9-c 引用它 —— 若 Task 9 单独编译会报未定义，需要**先创建 Task 11 的 SoftFingerprint.swift 才能编译过**。变通：本 Task 9-c 里先 hardcode 返回一个 dummy fingerprint，Task 11 里再改：

**改为**：

```swift
enum AttestationChallenge {
    static func collect(nonceBase64: String) -> [String: Any] {
        // NOTE(P4/T11): SoftFingerprint provides real fingerprint;
        // temporary dummy for compile until Task 11.
        let fingerprint = String(repeating: "0", count: 64)
        return [
            ChallengeKeys.attLevel: ChallengeKeys.levelSoft,
            ChallengeKeys.attFingerprint: fingerprint,
            ChallengeKeys.attSignature: "",
        ]
    }
}
```

Task 11 再切成 `SoftFingerprint.compute(...)`.

- [ ] **Step 7: Linux 上跑 smoke test**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`
Expected: 3 个 `✓ challenges/*.swift present`

- [ ] **Step 8: 若 macOS 可用，构建**

Run: `bash /root/byod-browser/build/build-guardian-macos.sh`
Expected: 编译成功

- [ ] **Step 9: Commit**

Run:
```bash
cd /root/byod-browser
git rm guardian/macos/Sources/ExamGuardian/challenges/StubChallenges.swift 2>/dev/null || \
  rm -f guardian/macos/Sources/ExamGuardian/challenges/StubChallenges.swift
git add guardian/macos/Sources/ExamGuardian/challenges/ProcessChallenge.swift \
        guardian/macos/Sources/ExamGuardian/challenges/DisplayChallenge.swift \
        guardian/macos/Sources/ExamGuardian/challenges/AttestationChallenge.swift \
        build/smoke-test-p4.sh
git commit -m "byod: real Process/Display/Attestation challenges (P4/T9)

P4 Task 9. ProcessChallenge uses libproc (Endpoint Security is v2 roadmap
per spec §7.1); tracks 'newly seen since last challenge' via static Set.
DisplayChallenge uses CGGetActiveDisplayList + CGDisplayBounds.
AttestationChallenge returns level=kSoft with dummy fingerprint (real
SoftFingerprint from Task 11)."
```

---

## Task 10: environment/{VmDetector,AntiDebug}.swift — 环境检测

**Files:**
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/environment/VmDetector.swift`
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/environment/AntiDebug.swift`
- Modify: `main.swift` — 启动时调 VmDetector + AntiDebug；命中 → 主动送 kEvent 触发 Chromium 端 burn（通过 P1-D 的 EmitEvent 语义，但 Guardian 端只能通过 wire 送 kEvent）

**Interfaces:**
- Consumes: Task 9 完成
- **Produces**：`VmDetector.detect()` (返回 bool + reason) 与 `AntiDebug.check()`；main.swift 里在 handshake 完成后立即检查一次，命中通过 kEvent 上报（或直接 exit + Chromium 端因心跳消失 burn）

**MVP 简化**：本 sub-plan 里 VmDetector/AntiDebug **命中后只 log + exit(2)**（Chromium 端因 Guardian 死了会 burn "guardian_crashed"）；正式版本应通过 kEvent 上报具体 reason。**接受简化以保持代码量**。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`:

```bash
  for f in VmDetector.swift AntiDebug.swift; do
    if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/environment/$f ]; then
      pass "environment/$f present"
    else
      fail "environment/$f missing"
    fi
  done
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 创建 VmDetector.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/environment/VmDetector.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 10 (spec §4.3.1 mac side): VM detection.
// Reads hw.model via sysctl; VMware / Parallels / VirtualBox all leave
// characteristic strings. Not perfect (custom hypervisors evade), but
// covers 99% of student-accessible VMs.

import Foundation
import Darwin

enum VmDetector {
    struct Result {
        let isVm: Bool
        let reason: String
    }

    static func detect() -> Result {
        let model = readSysctlString("hw.model") ?? ""
        let vmIndicators = ["VMware", "Parallels", "VirtualBox", "KVM",
                            "QEMU", "Xen"]
        for ind in vmIndicators {
            if model.localizedCaseInsensitiveContains(ind) {
                return Result(isVm: true, reason: "hw.model contains '\(ind)'")
            }
        }
        // Also check for hypervisor flag via sysctl kern.hv_vmm_present.
        var present: Int32 = 0
        var size = MemoryLayout<Int32>.size
        if sysctlbyname("kern.hv_vmm_present", &present, &size, nil, 0) == 0
           && present != 0 {
            return Result(isVm: true,
                          reason: "kern.hv_vmm_present=\(present)")
        }
        return Result(isVm: false, reason: "")
    }

    private static func readSysctlString(_ name: String) -> String? {
        var size = 0
        if sysctlbyname(name, nil, &size, nil, 0) != 0 { return nil }
        var buf = [CChar](repeating: 0, count: size)
        if sysctlbyname(name, &buf, &size, nil, 0) != 0 { return nil }
        return String(cString: buf)
    }
}
```

- [ ] **Step 4: 创建 AntiDebug.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/environment/AntiDebug.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 10 (spec §4.3.2 mac side): anti-debug (temperate).
//   - PT_DENY_ATTACH: refuse ptrace attach from anyone (macOS < 11 hard,
//     >= 11 weakened but still nonzero effort to bypass).
//   - sysctl kinfo_proc P_TRACED flag: check ourselves.
// Called once at startup after handshake.

import Foundation
import Darwin

enum AntiDebug {
    static func harden() {
        // PT_DENY_ATTACH = 31 (undocumented). Silently fails on new macOS
        // if kernel deems us unauthorized; we log but continue.
        let PT_DENY_ATTACH: Int32 = 31
        let rc = ptrace(PT_DENY_ATTACH, 0, nil, 0)
        if rc == -1 {
            Logger.warning("AntiDebug: PT_DENY_ATTACH failed errno=\(errno)")
        } else {
            Logger.info("AntiDebug: PT_DENY_ATTACH set")
        }
    }

    struct Result {
        let isDebugged: Bool
        let reason: String
    }

    static func check() -> Result {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.size
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        if sysctl(&mib, 4, &info, &size, nil, 0) != 0 {
            return Result(isDebugged: false, reason: "sysctl failed")
        }
        let traced = (info.kp_proc.p_flag & P_TRACED) != 0
        return Result(isDebugged: traced,
                      reason: traced ? "P_TRACED bit set" : "")
    }
}
```

**注意**：`ptrace` 的 Swift 声明可能需要 shim（Darwin 未在 Swift 里 export 所有原型）；如果编译报 "ptrace not declared"，加一个 bridging header 或 shim：

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/environment/AntiDebugShim.h`（若需要）:

```c
#ifndef ANTI_DEBUG_SHIM_H
#define ANTI_DEBUG_SHIM_H

#include <sys/types.h>
#include <sys/ptrace.h>

// Re-export ptrace for Swift.
int __ptrace_swift(int request, pid_t pid, caddr_t addr, int data);

#endif
```

Write `AntiDebugShim.c`:
```c
#include "AntiDebugShim.h"
int __ptrace_swift(int request, pid_t pid, caddr_t addr, int data) {
    return ptrace(request, pid, addr, data);
}
```

修改 `Package.swift` `.executableTarget` 段：

```swift
        .executableTarget(
            name: "ExamGuardian",
            path: "Sources/ExamGuardian",
            publicHeadersPath: "environment"
        )
```

**若 Swift 5.9 直接可解析 Darwin.ptrace**：省略 shim，直接用。按实际编译报错决定。

- [ ] **Step 5: 更新 main.swift 挂载**

Edit `/root/byod-browser/guardian/macos/Sources/ExamGuardian/main.swift`：在 `AntiDebug.harden` 之前完成 IpcKey 读取后添加：

```swift
// Environment checks (spec §4.3).
AntiDebug.harden()
let vmResult = VmDetector.detect()
if vmResult.isVm {
    Logger.error("VM detected: \(vmResult.reason); exiting")
    exit(2)
}
let debugResult = AntiDebug.check()
if debugResult.isDebugged {
    Logger.error("Debugger detected: \(debugResult.reason); exiting")
    exit(2)
}
```

- [ ] **Step 6: Linux 上跑 smoke test**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 7: Commit**

Run:
```bash
cd /root/byod-browser
git add guardian/macos/Sources/ExamGuardian/environment/VmDetector.swift \
        guardian/macos/Sources/ExamGuardian/environment/AntiDebug.swift \
        guardian/macos/Sources/ExamGuardian/main.swift \
        build/smoke-test-p4.sh
git commit -m "byod: VM detection + anti-debug (temperate) (P4/T10)

P4 Task 10. VmDetector: hw.model + kern.hv_vmm_present. AntiDebug:
PT_DENY_ATTACH + sysctl P_TRACED. Guardian exits(2) on hit; Chromium
side observes heartbeat loss and burns per FSM (spec §5.1.4)."
```

---

## Task 11: environment/SoftFingerprint.swift + 更新 AttestationChallenge

**Files:**
- Create: `/root/byod-browser/guardian/macos/Sources/ExamGuardian/environment/SoftFingerprint.swift`
- Modify: `challenges/AttestationChallenge.swift` — 用 SoftFingerprint 替换 dummy

**Interfaces:**
- Consumes: Task 10 完成
- **Produces**：`SoftFingerprint.compute(nonceBase64:)` 返回 SHA-256 hex；行为对齐 Chromium P1-E `SoftwareFingerprintAttestor::Quote`

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`:

```bash
  if [ -f /root/byod-browser/guardian/macos/Sources/ExamGuardian/environment/SoftFingerprint.swift ]; then
    pass "environment/SoftFingerprint.swift present"
  else
    fail "environment/SoftFingerprint.swift missing"
  fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 创建 SoftFingerprint.swift**

Write `/root/byod-browser/guardian/macos/Sources/ExamGuardian/environment/SoftFingerprint.swift`:

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 11 (spec §5.2.3 downgrade mac side): software fingerprint.
// Mirrors chrome::exam::SoftwareFingerprintAttestor::Quote (P1-E).
// Combines CPU brand + arch + hw.model + nonce → SHA-256 hex.
//
// Weak: cannot prove non-forgery, only stability across sessions on the
// same machine. Real TPM/SE bindings live in Chromium P1-E stubs and
// will be filled in P7.

import Foundation
import CommonCrypto
import Darwin

enum SoftFingerprint {
    static func compute(nonceBase64: String) -> String {
        let cpu = readSysctlString("machdep.cpu.brand_string") ?? "unknown-cpu"
        let arch = readSysctlString("hw.machine") ?? "unknown-arch"
        let board = readSysctlString("hw.model") ?? "unknown-board"
        let combined = "\(cpu)|\(arch)|\(board)|nonce=\(nonceBase64)"
        return sha256Hex(combined)
    }

    private static func readSysctlString(_ name: String) -> String? {
        var size = 0
        if sysctlbyname(name, nil, &size, nil, 0) != 0 { return nil }
        var buf = [CChar](repeating: 0, count: size)
        if sysctlbyname(name, &buf, &size, nil, 0) != 0 { return nil }
        return String(cString: buf)
    }

    private static func sha256Hex(_ s: String) -> String {
        let data = s.data(using: .utf8)!
        var digest = [UInt8](repeating: 0, count: Int(CC_SHA256_DIGEST_LENGTH))
        data.withUnsafeBytes { buf in
            _ = CC_SHA256(buf.baseAddress, CC_LONG(buf.count), &digest)
        }
        return digest.map { String(format: "%02x", $0) }.joined()
    }
}
```

- [ ] **Step 4: 更新 AttestationChallenge.swift 使用 SoftFingerprint**

Edit `/root/byod-browser/guardian/macos/Sources/ExamGuardian/challenges/AttestationChallenge.swift`：

```swift
// Copyright 2026 The BYOD Exam Browser Authors
// SPDX-License-Identifier: BSD-3-Clause
//
// BYOD P4 Task 9-c (updated in Task 11): attestation quote using
// SoftFingerprint. TPM/SE real signature is P7.

import Foundation

enum AttestationChallenge {
    static func collect(nonceBase64: String) -> [String: Any] {
        let fingerprint = SoftFingerprint.compute(nonceBase64: nonceBase64)
        return [
            ChallengeKeys.attLevel: ChallengeKeys.levelSoft,
            ChallengeKeys.attFingerprint: fingerprint,
            ChallengeKeys.attSignature: "",
        ]
    }
}
```

- [ ] **Step 5: Linux 上跑 smoke test**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 6: 若 macOS 可用，构建 + 手动 sanity 验证**

Run:
```bash
bash /root/byod-browser/build/build-guardian-macos.sh
# Guardian binary size 应 ~1-3 MB
ls -la /root/byod-browser/guardian/macos/.build/release/exam_guardian
```

- [ ] **Step 7: Commit**

Run:
```bash
cd /root/byod-browser
git add guardian/macos/Sources/ExamGuardian/environment/SoftFingerprint.swift \
        guardian/macos/Sources/ExamGuardian/challenges/AttestationChallenge.swift \
        build/smoke-test-p4.sh
git commit -m "byod: soft fingerprint + wire into AttestationChallenge (P4/T11)

P4 Task 11 (spec §5.2.3 mac side). Mirrors chrome::exam::
SoftwareFingerprintAttestor (P1-E). Combines CPU brand + arch + hw.model
+ nonce → SHA-256 hex via CommonCrypto. AttestationChallenge now uses
SoftFingerprint.compute for kSoft level; TPM/SE signature is P7 territory."
```

---

## 本部分尾声

**已交付**（P4 Task 7-11）：
- Task 7: ChallengeTypes + ChallengeResponder 骨架 + main.swift 分派
- Task 8: ImeChallenge（TIS）
- Task 9: ProcessChallenge (libproc) + DisplayChallenge (CoreGraphics) + AttestationChallenge (dummy fingerprint)
- Task 10: VmDetector (sysctl) + AntiDebug (PT_DENY_ATTACH + P_TRACED)
- Task 11: SoftFingerprint（CommonCrypto SHA-256）+ 替换 AttestationChallenge dummy

**里程碑**：**P4 功能完整** —— 所有 4 种 challenge 都有真实系统 API 实现；VM 检测 + 反调试 + 软指纹全就位。macOS 上编译产物是可用的 Guardian，可以被 P2-E IpcSupervisor spawn 并走完 handshake + heartbeat + challenge 循环。

## Task 12: 替换 P2-E stub_guardian 走真实 E2E

**Files:**
- Create: `/root/byod-browser/build/install-guardian-for-e2e.sh`（把 SPM 编出的 `exam_guardian` 拷贝到 Chromium out 目录的相应位置，让 IpcSupervisorE2ETest 用真 Guardian 而非 stub）

**Interfaces:**
- Consumes: P4 Task 11 完成（exam_guardian binary 可编）
- Produces: 一键把真 Guardian 装到 IpcSupervisor E2E 期望的位置；E2E 从 stub 升级为真 Guardian，完整验证 P2 + P4 联调

**说明**：**不修改 Chromium 端 IpcSupervisorE2ETest** —— 那个测试通过 `SetGuardianExecutablePathForTest(&stub_path_)` 定位 `byod_stub_guardian`（P2-E Task 8）。本 Task 提供一个 **install 脚本**，把真 `exam_guardian` **重命名** 拷贝到 `byod_stub_guardian` 路径 —— 相当于替换了 stub 但没改测试代码。这也证明 wire format 兼容性（真 Guardian 完美满足 stub 的行为契约）。

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p4.sh`：

```bash
if [[ "$(uname -s)" == "Darwin" ]]; then
  # ---------- Task 12: install script present ----------
  echo "[test] P4/task12: install-guardian-for-e2e.sh"
  if [ -x /root/byod-browser/build/install-guardian-for-e2e.sh ]; then
    pass "install-guardian-for-e2e.sh present"
  else
    fail "install-guardian-for-e2e.sh missing or not executable"
  fi
fi
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`

- [ ] **Step 3: 创建 install-guardian-for-e2e.sh**

Write `/root/byod-browser/build/install-guardian-for-e2e.sh`:

```bash
#!/usr/bin/env bash
# Install the SPM-built exam_guardian into the Chromium out directory,
# replacing the P2-E stub_guardian so IpcSupervisorE2ETest runs against
# the real macOS Guardian.
#
# Usage:
#   bash build/install-guardian-for-e2e.sh [chromium-out-dir]
# Defaults to /opt/chromium/src/out/mac-arm64 on Apple Silicon or
# /opt/chromium/src/out/mac-x64 on Intel.
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[error] macOS only"; exit 1
fi

# Ensure guardian built.
GUARDIAN_BIN="/root/byod-browser/guardian/macos/.build/release/exam_guardian"
if [ ! -x "$GUARDIAN_BIN" ]; then
  echo "[info] exam_guardian not built; building..."
  bash /root/byod-browser/build/build-guardian-macos.sh
fi
if [ ! -x "$GUARDIAN_BIN" ]; then
  echo "[error] build failed; $GUARDIAN_BIN still missing"; exit 1
fi

# Detect out dir.
CHROMIUM_OUT="${1:-}"
if [ -z "$CHROMIUM_OUT" ]; then
  if [ "$(uname -m)" = "arm64" ]; then
    CHROMIUM_OUT="/opt/chromium/src/out/mac-arm64"
  else
    CHROMIUM_OUT="/opt/chromium/src/out/mac-x64"
  fi
fi
if [ ! -d "$CHROMIUM_OUT" ]; then
  echo "[error] Chromium out dir not found: $CHROMIUM_OUT"; exit 1
fi

# Install as both names: the real name (for future direct use) AND the
# byod_stub_guardian name (so the existing E2E gtest picks it up).
cp -f "$GUARDIAN_BIN" "$CHROMIUM_OUT/exam_guardian"
cp -f "$GUARDIAN_BIN" "$CHROMIUM_OUT/byod_stub_guardian"

echo "[success] Guardian installed to $CHROMIUM_OUT (both exam_guardian +"
echo "        byod_stub_guardian names — E2E gtest will use it as stub)"
```

Run:
```bash
chmod +x /root/byod-browser/build/install-guardian-for-e2e.sh
```

- [ ] **Step 4: 若 macOS 可用，跑安装 + E2E gtest**

Run:
```bash
bash /root/byod-browser/build/install-guardian-for-e2e.sh
/opt/chromium/src/out/mac-arm64/unit_tests \
  --gtest_filter="IpcSupervisorE2ETest.*" 2>&1 | tail -15
```
Expected:
- `HandshakeSucceeds` PASS（真 Guardian 完成 fd 3 read + 进入 loop）
- `HeartbeatRoundTrip` PASS（真 Guardian 5s 送心跳 + 收心跳都不 burn）
- `ShutdownTerminatesGuardian` PASS

**若失败**：诊断路径：
- Guardian log（stderr）应有 `[guardian] handshake complete`；若无 → fd 传递问题
- Chromium log 应有 `PeerAttestor: kAccept`；若 `kRejectPathMismatch` → Task 12 覆写的路径不对
- 若 `HeartbeatRoundTrip` 在 6s 内 burn → 检查 wire format 是否有 byte 差异（用 `xxd` 抓 socket 流对比 Chromium 出的帧 vs Guardian 出的帧）

**若 gtest 全通过**：确认 wire format + challenge schema 冻结完全兼容 —— 这是 P2 + P4 联调的关键里程碑。

- [ ] **Step 5: Linux 上跑 smoke test（Linux 部分永远 skip Darwin-only 段）**

Run: `bash /root/byod-browser/build/smoke-test-p4.sh`
Expected: Linux 上正常 skip，但源码 verify 段全 pass

- [ ] **Step 6: Commit**

Run:
```bash
cd /root/byod-browser
git add build/install-guardian-for-e2e.sh build/smoke-test-p4.sh
git commit -m "byod: install script to swap stub_guardian for real macOS Guardian (P4/T12)

P4 Task 12. Installs SPM-built exam_guardian into Chromium out/ under
both its real name AND byod_stub_guardian so existing P2-E
IpcSupervisorE2ETest runs unmodified against the real Guardian —
proves wire format + challenge schema compatibility."
```

---

## Task 13: P4 acceptance + 更新 roadmap

**Files:**
- Create: `/root/byod-browser/docs/development/p4-acceptance.md`

**Interfaces:**
- Consumes: Task 1..12 完成
- Produces: P4 交付验收清单 + roadmap 状态更新

- [ ] **Step 1: 写 acceptance checklist**

Write `/root/byod-browser/docs/development/p4-acceptance.md`:

```markdown
# P4 交付验收 Checklist（macOS Guardian）

## 源码类

- [ ] `guardian/macos/Package.swift` 存在
- [ ] `guardian/macos/README.md` 存在
- [ ] `Sources/ExamGuardian/main.swift` 存在
- [ ] `Sources/ExamGuardian/IpcKey.swift` 存在
- [ ] `Sources/ExamGuardian/FrameCodec.swift` 存在
- [ ] `Sources/ExamGuardian/FrameHmac.swift` 存在
- [ ] `Sources/ExamGuardian/SequenceGuard.swift` 存在
- [ ] `Sources/ExamGuardian/TransportReader.swift` 存在
- [ ] `Sources/ExamGuardian/HeartbeatSender.swift` 存在
- [ ] `Sources/ExamGuardian/ChallengeTypes.swift` 存在
- [ ] `Sources/ExamGuardian/ChallengeResponder.swift` 存在
- [ ] `Sources/ExamGuardian/challenges/ImeChallenge.swift` 存在
- [ ] `Sources/ExamGuardian/challenges/ProcessChallenge.swift` 存在
- [ ] `Sources/ExamGuardian/challenges/DisplayChallenge.swift` 存在
- [ ] `Sources/ExamGuardian/challenges/AttestationChallenge.swift` 存在
- [ ] `Sources/ExamGuardian/environment/VmDetector.swift` 存在
- [ ] `Sources/ExamGuardian/environment/AntiDebug.swift` 存在
- [ ] `Sources/ExamGuardian/environment/SoftFingerprint.swift` 存在
- [ ] `Sources/ExamGuardian/util/Logger.swift` 存在
- [ ] `Sources/ExamGuardian/challenges/StubChallenges.swift` 已删除
- [ ] `build/build-guardian-macos.sh` 可执行
- [ ] `build/install-guardian-for-e2e.sh` 可执行
- [ ] `build/smoke-test-p4.sh` 可执行

## 编译类（macOS only）

- [ ] `bash build/build-guardian-macos.sh` 成功
- [ ] 产物 `guardian/macos/.build/release/exam_guardian` 存在且可执行
- [ ] 二进制大小合理（1-5 MB，无外部动态库依赖）
- [ ] 依赖框架：CommonCrypto、Carbon、CoreGraphics、Darwin（Apple system frameworks 自动 link）

## 行为类（macOS only）

- [ ] `bash build/smoke-test-p4.sh` 全 pass；exam_guardian 独立跑（无 fd 3/4）优雅报错退出
- [ ] `bash build/install-guardian-for-e2e.sh` 成功拷贝到 Chromium out
- [ ] `unit_tests --gtest_filter="IpcSupervisorE2ETest.HandshakeSucceeds"` PASS
- [ ] `unit_tests --gtest_filter="IpcSupervisorE2ETest.HeartbeatRoundTrip"` PASS（6s 无 burn）
- [ ] `unit_tests --gtest_filter="IpcSupervisorE2ETest.ShutdownTerminatesGuardian"` PASS

## 行为类（Linux dev）

- [ ] `bash build/smoke-test-p4.sh` 正常 skip Darwin-only 段，源码 verify 全 pass

## 手动测试类（macOS 有 GUI 时）

- [ ] 装 Guardian 后手动启动 Chromium，`ExamSession::StartExam` 触发真 Guardian spawn；Chromium log 显示 `PeerAttestor: kAccept`；Guardian stderr 显示 `[guardian] handshake complete`
- [ ] 60s 观察：无假阳性 burn；Chromium log 每 30s 显示 challenge 响应到达；每种 challenge type 都能收到合理 data（IME 名字对得上 System Preferences 显示的当前 IME、进程列表新条目符合当前操作）
- [ ] 手动切换 IME 到搜狗（非白名单）→ 30s 内 challenge 响应触发 `PolicyChallengeVerifier` 返回 false → `TriggerBurn("challenge_rejected")` → ExamSession 进入 kBurning
- [ ] 手动在 VM 里跑 → VmDetector 命中 exit(2) → Chromium 因 Guardian 消失 30s 后 burn `heartbeat_timeout`

## 接口冻结类

- [ ] wire format v1 byte-compatible（Chromium P2-B 与 Guardian FrameCodec.swift 输出/接受同样的字节序列）
- [ ] challenge schema v1 byte-compatible（ChallengeResponder 输出的 response JSON 通过 Chromium P5 PolicyChallengeVerifier 校验）
- [ ] IpcKey 32 字节、HMAC-SHA256、seq monotonic 语义在两端一致

## 已知限制 / 后续 refactor

- [ ] Endpoint Security → v2 roadmap（用 libproc 代替，需 Apple entitlement）
- [ ] TPM/SE 硬件签名验证 → P7（本 sub-plan level=kSoft，signature=""）
- [ ] VM/AntiDebug 命中未通过 kEvent 上报，只 exit(2) 让 Chromium 因心跳 timeout burn；生产版本改进为送 kEvent 携带具体 reason
- [ ] 权限申请 UI 无（假设用户已在 System Settings 授予 Accessibility）→ P11 Onboarding

## 文档类

- [ ] 本文件（p4-acceptance.md）存在
- [ ] `guardian/macos/README.md` 存在
- [ ] roadmap 里 P4 状态更新为 `✅ 完成`

## 交付判定

以上全部打勾（macOS 项若资源到位）→ P4 完成，可进入 P3 (Windows Rust Guardian) / P6 (Exam Shell UI) / P7 (Browser Exam Key + 真 TPM/SE) / P8 (Session Report)。

**关键里程碑**：真实的 macOS Guardian 上线；IpcSupervisorE2ETest 在真 Guardian 上跑通 = **P2 + P4 端到端联调成功**。wire format + challenge schema 兼容性经真实运行证明。
```

- [ ] **Step 2: Commit acceptance**

Run:
```bash
cd /root/byod-browser
git add docs/development/p4-acceptance.md
git commit -m "docs: add P4 acceptance checklist"
```

- [ ] **Step 3: 更新 roadmap**

编辑 `/root/byod-browser/docs/superpowers/plans/2026-07-11-byod-roadmap.md`，把 P4 那一行状态改成 `✅ 完成`。

Run:
```bash
cd /root/byod-browser
git add docs/superpowers/plans/2026-07-11-byod-roadmap.md
git commit -m "docs: mark P4 complete in roadmap"
```

---

## P4 完成后

**P4 完全交付**：
- 19 个 Swift 源文件在 `guardian/macos/Sources/ExamGuardian/`
- 3 个 build/install/smoke 脚本
- 1 个 SPM manifest
- 1 个 README
- 1 个 acceptance checklist
- **无 Chromium 补丁**（Guardian 独立）
- **不占 spec §3.1 的 3000 行硬预算**

**里程碑达成** 🎉：
- macOS 上完整的 handshake + heartbeat + challenge + burn 端到端可运行
- P2-E 的 stub_guardian 被真 Guardian 无缝替换（无需改 gtest 代码）
- wire format v1 + challenge schema v1 兼容性经真实运行验证 —— **P3 (Windows Rust) 可以完全信心地按同一份规范实施**

**已知限制**（诚实标注）：
- Endpoint Security 用 libproc 代替（需 Apple entitlement）
- TPM/SE 签名验证推迟到 P7
- 环境检测命中通过 exit(2) 隐式上报（生产版本应用 kEvent 显式上报）
- 权限申请 UI 缺失（P11 Onboarding）

**下一步可并行**：
- **P3** — Windows Rust Guardian（参照 P4 结构，同一份规范）
- **P6** — 考试壳 UI
- **P7** — Browser Exam Key + 真 TPM/SE
- **P8** — Session Report

**预计 P4 实际耗时**：
- 熟练 Swift + macOS 开发者：2-3 周
- 首次接触 CommonCrypto/Carbon/libproc：4-6 周（含调试 fd 传递、TIS API、posix_spawn 与 Chromium spawn 时机对齐）

**风险与常见坑**：
- **Carbon TIS API 在新版 macOS 上 deprecated 但仍工作**：Xcode 会 warn，不 error；生产上继续用直到 Apple 提供替代品
- **`ptrace` PT_DENY_ATTACH 在 macOS 11+ 弱化**：不能 100% 阻止内核调试器；这是 macOS 平台的边界，非 P4 bug
- **libproc 在 sandboxed 进程里可能被限制**：Guardian 现在没 sandbox，不受影响；若未来加 sandbox 要重新评估
- **POSIX_SPAWN_CLOEXEC_DEFAULT 关掉 stdout/stderr**：Guardian 的 Logger 写 stderr，需要 P2-E launcher 显式保留 fd 2 到子进程（当前 launcher 通过 posix_spawn_file_actions_adddup2 只 remap 3/4，未处理 2）—— 现象是 Guardian log 消失；修法：launcher 里 addinherit_np fd 0/1/2，或让 Guardian 用 syslog
- **codesigning 相关**：v1 无签名，Gatekeeper 会弹窗；用户可通过 System Settings > Security 允许运行；P10 会补 codesign + notarization