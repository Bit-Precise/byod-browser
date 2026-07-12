# P1-A · Kiosk & Scheme 隔离补丁 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 合入 6 个"删/禁"类 Chromium 补丁：编译期禁 DevTools 入口 + 命令行开关无效化 + scheme 白名单收紧 + 开发者/截屏快捷键劫持，覆盖 spec §3.1 表中的补丁 #1、#2、#3、#4、#10、#11。

**Architecture:** 所有修改通过 `git format-patch` 导出到 `patches/00XX-*.patch`，用 P0 的 `apply-patches.sh` 应用；每个补丁独立一个 git commit 一个 `.patch` 文件；每合入一个补丁跑一次 build + 一份专门的 smoke test 验证行为。

**Tech Stack:** Chromium (C++) + gn/ninja + shell smoke tests；Chromium 源码位置 `/opt/chromium/src`（P0 已就绪）。

## Global Constraints

- Chromium 补丁总量 ≤ 3000 行（全 P1 累计；本批预算 ≤ 600 行）
- Chromium 补丁只做"删/禁"和"钩子暴露"，不写业务
- 每个补丁必须独立可 revert；即使中间某一个 revert 掉，Chromium 也能编译并启动
- 文件路径全部相对 `/opt/chromium/src/`（Chromium 源码约定），补丁文件本身相对 `/root/byod-browser/patches/`
- 每个补丁的 commit message 前缀固定 `byod: `；spec 章节引用固定 `see spec §3.1 patch #N`
- **保留 CDP 内部通信路径**：只切用户可访问的 DevTools 入口，Chromium 内部（Print Preview、Reader Mode、Accessibility）仍能用 CDP（spec §4.1.3 已知边界）
- Chromium 源码 pin 版本参照 `/root/byod-browser/build/chromium-version.env`（P0 Task 2 写入的 `CHROMIUM_TAG`，示例 `130.0.6723.116`）

---

## File Structure

**本 sub-plan 交付：**

```
/root/byod-browser/
├── patches/
│   ├── 0001-disable-devtools-window-entrypoints.patch     # 补丁 #1a
│   ├── 0002-noop-open-devtools-shortcut.patch             # 补丁 #1b (F12 / Ctrl+Shift+I)
│   ├── 0003-strip-remote-debugging-switches.patch         # 补丁 #2
│   ├── 0010-strip-view-source-shortcut.patch              # 补丁 #10a (Ctrl+U)
│   ├── 0011-strip-save-and-print-shortcuts.patch          # 补丁 #10b (Ctrl+S, Ctrl+P)
│   ├── 0100-restrict-webui-scheme-to-exam-shell.patch     # 补丁 #3
│   ├── 0101-block-file-and-view-source-schemes.patch      # 补丁 #4a
│   ├── 0102-block-chrome-devtools-and-extension-schemes.patch  # 补丁 #4b
│   └── 0500-block-macos-screenshot-shortcuts.patch        # 补丁 #11 (mac only)
├── build/
│   └── smoke-test-p1a.sh                                  # 本批专用行为验证脚本
└── docs/
    └── development/
        └── p1-a-acceptance.md                             # 本批验收清单
```

**Chromium 源码触及区域（供 review 追溯）：**

- `chrome/browser/devtools/devtools_window.cc` (#1a)
- `chrome/browser/ui/views/accelerator_table.cc` + `chrome/app/chrome_command_ids.h` (#1b, #10a, #10b)
- `content/public/common/content_switches.cc` + `content/browser/devtools/devtools_http_handler.cc` (#2)
- `chrome/browser/ui/webui/chrome_web_ui_controller_factory.cc` + `chrome/common/webui_url_constants.cc` (#3)
- `content/browser/child_process_security_policy_impl.cc` + `chrome/browser/browser_url_handler_impl.cc` (#4a, #4b)
- `chrome/browser/global_keyboard_shortcuts_mac.mm` + `chrome/browser/app_controller_mac.mm` (#11)

**注意**：以上路径是 Chromium 130 stable 版本的典型位置，rebase 到新版本时可能 rename/move —— 是 P0 rebase-guide.md 处理的场景。

---

## Task 1: 补丁 #1a — 禁用 DevTools 窗口创建入口

**Files:**
- Modify (in Chromium src): `chrome/browser/devtools/devtools_window.cc`
- Create: `/root/byod-browser/patches/0001-disable-devtools-window-entrypoints.patch`
- Create: `/root/byod-browser/build/smoke-test-p1a.sh`（首次创建）

**Interfaces:**
- Consumes: P0 完成（`bash build/apply-patches.sh` / `revert-patches.sh` / `build-linux.sh` / `smoke-test.sh` 都可用）
- Produces: `chrome://exam-shell` 内触发"打开 DevTools"命令时静默 no-op；DevTools 相关的 `chrome://` 内部页面（`chrome://devtools`）也无法打开

- [ ] **Step 1: 起始状态自检**

Run:
```bash
cd /opt/chromium/src
git status
git describe --tags
```
Expected: `working tree clean`；tag 与 `chromium-version.env` 的 `CHROMIUM_TAG` 一致。若不干净：`bash /root/byod-browser/build/revert-patches.sh` 再来。

- [ ] **Step 2: 定位 DevTools 窗口入口函数**

Run:
```bash
grep -n "^DevToolsWindow\* DevToolsWindow::OpenDevTools" /opt/chromium/src/chrome/browser/devtools/devtools_window.cc | head -5
grep -n "^void DevToolsWindow::ToggleDevToolsWindow" /opt/chromium/src/chrome/browser/devtools/devtools_window.cc | head -5
```
Expected: 两个函数各有一行输出，记录行号。**若行号与本文档预设有偏差**（Chromium 版本差异），按 grep 出的实际行号调整下面的 patch。

- [ ] **Step 3: 写 smoke test 先定义"期望行为"（TDD）**

Write `/root/byod-browser/build/smoke-test-p1a.sh`（这是整个 P1-A 共用的行为验证脚本，本任务只写它的骨架 + 补丁 #1a 相关的测试段；后续任务会向此脚本追加更多测试段）:

```bash
#!/usr/bin/env bash
# Behavior smoke tests for P1-A patches (#1..#4, #10, #11).
# Run after applying patches and rebuilding chrome.
# Each subtest is idempotent and independent.
set -euo pipefail

CHROME_BIN="${1:-/opt/chromium/src/out/linux/chrome}"
if [ ! -x "$CHROME_BIN" ]; then
  echo "[error] chrome not found: $CHROME_BIN"; exit 1
fi

CHROME_TMP="$(mktemp -d)"
trap 'rm -rf "$CHROME_TMP"' EXIT

run_chrome() {
  # Common args: fresh profile, headless-ish, disable sandbox for CI containers,
  # short timeout to prevent hangs.
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

# ---------- Patch #1a: DevTools window entry disabled ----------
echo "[test] patch #1a: DevTools window entry"

# Chromium exposes an about://inspect page that lists debuggable targets.
# With #1a in place, opening chrome://inspect should NOT open a DevTools panel.
# We check by launching with --auto-open-devtools-for-tabs and asserting no
# devtools panel opens (via absence of the DevTools remote debugging endpoint
# artifact — but #2 disables that too; here we check the process count).
out="$(run_chrome --headless --auto-open-devtools-for-tabs --dump-dom https://example.com 2>&1 || true)"
if echo "$out" | grep -q -i "devtools_frontend_host\|devtools://"; then
  fail "#1a: --auto-open-devtools-for-tabs still triggers devtools frontend"
else
  pass "#1a: --auto-open-devtools-for-tabs is a no-op"
fi

# ---------- Additional patch tests appended in later tasks ----------

if [ "$FAILED" -ne 0 ]; then
  echo "[fail] one or more P1-A tests failed"; exit 1
fi
echo "[success] all P1-A tests passed"
```

Run:
```bash
chmod +x /root/byod-browser/build/smoke-test-p1a.sh
```

- [ ] **Step 4: 在 vanilla Chromium 上运行 smoke test，确认它当前失败**

Run:
```bash
bash /root/byod-browser/build/smoke-test-p1a.sh /opt/chromium/src/out/linux/chrome
```
Expected: `✗ #1a: --auto-open-devtools-for-tabs still triggers devtools frontend` → `[fail] one or more P1-A tests failed`

（这一步证明测试确实能检测"没有补丁的 vanilla Chromium"）

- [ ] **Step 5: 在 devtools_window.cc 里改函数体**

在 `/opt/chromium/src/chrome/browser/devtools/devtools_window.cc` 里找到 `DevToolsWindow::OpenDevTools` 和 `DevToolsWindow::ToggleDevToolsWindow` 两个函数（Step 2 已定位）。

用 Edit 把两个函数的函数体改为立即 return（保留签名不动），并在文件顶部（`#include` 块之后、匿名 namespace 之前）加一条 BYOD 标记注释。

**具体改动内容**（示例；实际行号以 Step 2 grep 结果为准）：

原始 `OpenDevTools`（函数体，从 `{` 到 `}`）替换为：

```cpp
{
  // BYOD patch #1a (spec §3.1): DevTools window entrypoints are disabled at
  // compile time. Internal CDP paths (Print Preview, Reader Mode, a11y) are
  // unaffected — only user-visible window creation is stubbed.
  return nullptr;
}
```

原始 `ToggleDevToolsWindow`（函数体）替换为：

```cpp
{
  // BYOD patch #1a (spec §3.1): see OpenDevTools comment above.
  return;
}
```

**用 Edit 工具做这两处替换**（不写完整 shell 命令，因为 Edit 依赖精确的原文匹配 —— 执行者要先 Read 该文件的对应行段，再对每个函数体做 Edit）。

- [ ] **Step 6: 增量编译**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
```
Expected: 增量编译（改动只在 chrome_browser target 内），10-30 分钟；最终 `[success] built: ...`

- [ ] **Step 7: 跑 P0 通用 smoke test 确认没弄坏基础功能**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
```
Expected: `[success] smoke test passed`

- [ ] **Step 8: 跑 P1-A smoke test 确认 #1a 现在通过**

Run:
```bash
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: `✓ #1a: --auto-open-devtools-for-tabs is a no-op` → `[success] all P1-A tests passed`

- [ ] **Step 9: 在 Chromium src 里 commit 这次改动**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/devtools/devtools_window.cc
git commit -m "byod: disable OpenDevTools/ToggleDevToolsWindow entrypoints

Patch #1a (spec §3.1). Compile-time stub of user-visible DevTools window
creation. Internal CDP paths remain functional for Print Preview, Reader
Mode, and Accessibility."
```

- [ ] **Step 10: 导出 patch 文件**

Run:
```bash
source /root/byod-browser/build/chromium-version.env
cd /opt/chromium/src
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0001-disable-devtools-window-entrypoints.patch
```

- [ ] **Step 11: 验证 revert → re-apply 循环可用**

Run:
```bash
echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
```
Expected: revert 成功、apply 报 `[success] all patches applied on top of <TAG>`

- [ ] **Step 12: 增量再编 + 再跑 smoke（确认 patch 文件本身可用，不是靠内存里的编辑）**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: 两者都成功

- [ ] **Step 13: Commit patch 到本 repo**

Run:
```bash
cd /root/byod-browser
git add patches/0001-disable-devtools-window-entrypoints.patch build/smoke-test-p1a.sh
git commit -m "patch: 0001 disable DevTools window entrypoints (patch #1a)

Implements spec §3.1 patch #1 first half. Adds P1-A smoke test harness.
Estimated LOC: ~30 (well under the 150-LOC budget for patch #1)."
```

---

## Task 2: 补丁 #1b — 移除 DevTools 快捷键映射

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/views/accelerator_table.cc`（Linux/Win）
- Modify (in Chromium src): `chrome/browser/global_keyboard_shortcuts_mac.mm`（macOS，若 Linux dev 环境编不到可先跳过 mac 部分）
- Create: `/root/byod-browser/patches/0002-noop-open-devtools-shortcut.patch`

**Interfaces:**
- Consumes: Task 1 完成
- Produces: F12、Ctrl+Shift+I（macOS: Cmd+Option+I）、Ctrl+Shift+J、Ctrl+Shift+C 快捷键从加速表里删除

- [ ] **Step 1: 定位快捷键表条目**

Run:
```bash
grep -n "IDC_DEV_TOOLS" /opt/chromium/src/chrome/browser/ui/views/accelerator_table.cc | head -20
```
Expected: 若干行输出，各条目形如 `{ IDC_DEV_TOOLS, ui::VKEY_I, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN },` 之类。记录所有 `IDC_DEV_TOOLS*` 相关的行号。

- [ ] **Step 2: 向 smoke test 追加 #1b 测试段**

Edit `/root/byod-browser/build/smoke-test-p1a.sh`，在 `# ---------- Additional patch tests appended in later tasks ----------` 行之前追加：

```bash
# ---------- Patch #1b: DevTools shortcuts removed ----------
echo "[test] patch #1b: DevTools keyboard shortcuts"

# Chromium's --enable-features=... path won't reveal keyboard shortcut wiring
# directly. Instead, we inspect the built binary for the presence of the
# accelerator entry symbol. After #1b, IDC_DEV_TOOLS should NOT appear in
# accelerator table static data.
if strings "$CHROME_BIN" 2>/dev/null | grep -q "AcceleratorTable_IDC_DEV_TOOLS"; then
  # Fallback textual check on a related string; if binary is stripped we skip
  echo "  (note: symbol-based test skipped; strings did not find marker)"
fi

# Behavioral test: launch a page and simulate F12 via --auto-open-devtools-for-tabs
# should already be covered by #1a; here we assert the *shortcut command* is unbound.
# We rely on chrome://accessibility to list registered accelerators.
out="$(run_chrome --headless --dump-dom chrome://accessibility 2>&1 || true)"
if echo "$out" | grep -qi "IDC_DEV_TOOLS"; then
  fail "#1b: IDC_DEV_TOOLS still registered in accessibility accelerator listing"
else
  pass "#1b: IDC_DEV_TOOLS not registered"
fi
```

- [ ] **Step 3: 跑一次确认新测试段目前失败**

Run:
```bash
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: `✗ #1b: IDC_DEV_TOOLS still registered ...`；`✓ #1a: ...`（Task 1 的仍然过）

- [ ] **Step 4: 在 accelerator_table.cc 里删条目**

用 Edit 工具，找到 Step 1 定位的所有 `{ IDC_DEV_TOOLS*, ... },` 结构体条目，**整行删除**（保留周围的 `#if` / `#endif` 结构）。

**举例** —— 若原文形如：

```cpp
    { IDC_DEV_TOOLS, ui::VKEY_I,
      ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN },
    { IDC_DEV_TOOLS_CONSOLE, ui::VKEY_J,
      ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN },
    { IDC_DEV_TOOLS_INSPECT, ui::VKEY_C,
      ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN },
    { IDC_DEV_TOOLS, ui::VKEY_F12, ui::EF_NONE },
```

改后：

```cpp
    // BYOD patch #1b (spec §3.1): DevTools accelerators removed.
    // { IDC_DEV_TOOLS / _CONSOLE / _INSPECT / F12 entries were here }
```

（用注释保留占位而不是完全删掉行，rebase 时更好看冲突）

- [ ] **Step 5: 增量编译**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
```
Expected: 成功

- [ ] **Step 6: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: 两个都成功

- [ ] **Step 7: 导出 patch**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/views/accelerator_table.cc
git commit -m "byod: strip DevTools accelerators from accelerator table

Patch #1b (spec §3.1). F12 / Ctrl+Shift+I / Ctrl+Shift+J / Ctrl+Shift+C
no longer bind to IDC_DEV_TOOLS commands."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0002-noop-open-devtools-shortcut.patch
```

- [ ] **Step 8: Revert + re-apply 循环验证**

Run:
```bash
echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: 全成功

- [ ] **Step 9: Commit**

Run:
```bash
cd /root/byod-browser
git add patches/0002-noop-open-devtools-shortcut.patch build/smoke-test-p1a.sh
git commit -m "patch: 0002 strip DevTools accelerators (patch #1b)

Second half of spec §3.1 patch #1. Total for #1 now ~50 LOC."
```

---

## Task 3: 补丁 #2 — 移除 `--remote-debugging-port` 与 `--remote-debugging-pipe` 命令行开关

**Files:**
- Modify (in Chromium src): `content/public/common/content_switches.cc` + `content/public/common/content_switches.h`
- Modify (in Chromium src): `content/browser/devtools/devtools_http_handler.cc`
- Create: `/root/byod-browser/patches/0003-strip-remote-debugging-switches.patch`

**Interfaces:**
- Consumes: Task 2 完成
- Produces: `--remote-debugging-port=9222` / `--remote-debugging-pipe` / `--remote-debugging-address` 全部失效；启动时忽略这些参数不报错也不生效

- [ ] **Step 1: 定位命令行开关声明**

Run:
```bash
grep -n "kRemoteDebugging" /opt/chromium/src/content/public/common/content_switches.h
grep -n "kRemoteDebugging" /opt/chromium/src/content/public/common/content_switches.cc
grep -rn "switches::kRemoteDebuggingPort" /opt/chromium/src/content/browser/devtools/ | head -5
```
Expected: 各自若干行；记录 header 里 `extern const char kRemoteDebuggingPort[]` 等声明所在行，cc 里的字符串定义 `const char kRemoteDebuggingPort[] = "remote-debugging-port";` 所在行，以及 `devtools_http_handler.cc` 里 `CommandLine::HasSwitch(switches::kRemoteDebuggingPort)` 所在行。

- [ ] **Step 2: 向 smoke test 追加 #2 测试段**

Edit `/root/byod-browser/build/smoke-test-p1a.sh`，在追加区之前继续追加：

```bash
# ---------- Patch #2: --remote-debugging-port is a no-op ----------
echo "[test] patch #2: --remote-debugging-port stripped"

# Launch chrome with --remote-debugging-port=9222 and check that no listener
# comes up on that port.
run_chrome --remote-debugging-port=9222 --headless https://example.com > /dev/null 2>&1 &
CHROME_PID=$!
sleep 3

if command -v ss >/dev/null 2>&1; then
  if ss -tln 2>/dev/null | grep -q ':9222 '; then
    fail "#2: port 9222 is listening despite patch"
  else
    pass "#2: port 9222 is not listening"
  fi
else
  if netstat -tln 2>/dev/null | grep -q ':9222 '; then
    fail "#2: port 9222 is listening despite patch"
  else
    pass "#2: port 9222 is not listening"
  fi
fi

kill $CHROME_PID 2>/dev/null || true
wait $CHROME_PID 2>/dev/null || true
```

- [ ] **Step 3: 跑一次确认失败**

Run:
```bash
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: `✗ #2: port 9222 is listening despite patch`

- [ ] **Step 4: 修改 devtools_http_handler.cc 让它早退**

在 `/opt/chromium/src/content/browser/devtools/devtools_http_handler.cc` 里找到 handler 构造/启动函数（`DevToolsHttpHandler::DevToolsHttpHandler(...)` 或 `Start()`），在函数入口最开始加：

```cpp
  // BYOD patch #2 (spec §3.1): remote debugging HTTP handler is disabled at
  // compile time. --remote-debugging-port / --remote-debugging-pipe are ignored.
  // Internal CDP dispatch (Print Preview, Reader Mode, a11y) uses in-process
  // paths that don't require the HTTP handler.
  return;
```

（如果早退位置在构造函数里，把返回改成"不启动 HTTP server"的效果 —— 具体形式看 Chromium 版本，可能需要设置一个成员变量 `started_ = false` 之后早返回）

**若 handler 的构造/启动分离**：只在 `Start()` / `StartOnUI()` 类函数里早退。

- [ ] **Step 5: 增量编译**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
```

- [ ] **Step 6: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: 都成功

- [ ] **Step 7: 导出 patch**

Run:
```bash
cd /opt/chromium/src
git add content/browser/devtools/devtools_http_handler.cc
git commit -m "byod: no-op DevToolsHttpHandler start path

Patch #2 (spec §3.1). --remote-debugging-port and --remote-debugging-pipe
now have no effect. Internal CDP dispatch is unaffected."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0003-strip-remote-debugging-switches.patch
```

- [ ] **Step 8: Revert + re-apply 循环验证**

Run:
```bash
echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: 全成功

- [ ] **Step 9: Commit**

Run:
```bash
cd /root/byod-browser
git add patches/0003-strip-remote-debugging-switches.patch build/smoke-test-p1a.sh
git commit -m "patch: 0003 no-op remote debugging handler (patch #2)

Estimated LOC: ~20."
```

---

## Task 4: 补丁 #10a — 移除 view-source 快捷键（Ctrl+U）

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/views/accelerator_table.cc`
- Create: `/root/byod-browser/patches/0010-strip-view-source-shortcut.patch`

**Interfaces:**
- Consumes: Task 3 完成
- Produces: Ctrl+U 不再打开 view-source

- [ ] **Step 1: 定位 IDC_VIEW_SOURCE 条目**

Run:
```bash
grep -n "IDC_VIEW_SOURCE" /opt/chromium/src/chrome/browser/ui/views/accelerator_table.cc
```

- [ ] **Step 2: 向 smoke test 追加 #10a 测试**

Edit `/root/byod-browser/build/smoke-test-p1a.sh` 追加：

```bash
# ---------- Patch #10a: view-source shortcut removed ----------
echo "[test] patch #10a: Ctrl+U (view-source) unbound"

out="$(run_chrome --headless --dump-dom chrome://accessibility 2>&1 || true)"
if echo "$out" | grep -qi "IDC_VIEW_SOURCE"; then
  fail "#10a: IDC_VIEW_SOURCE still bound"
else
  pass "#10a: IDC_VIEW_SOURCE not bound"
fi
```

- [ ] **Step 3: 跑一次确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1a.sh`

- [ ] **Step 4: 删掉条目**

用 Edit 找到 `{ IDC_VIEW_SOURCE, ui::VKEY_U, ui::EF_CONTROL_DOWN },`（版本差异导致的 modifier 组合可能略有不同）整行注释掉：

```cpp
    // BYOD patch #10a (spec §3.1): IDC_VIEW_SOURCE accelerator removed.
```

- [ ] **Step 5: 编译 + smoke**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
```

- [ ] **Step 6: 导出 patch + revert/re-apply 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/views/accelerator_table.cc
git commit -m "byod: strip IDC_VIEW_SOURCE accelerator

Patch #10a (spec §3.1)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0010-strip-view-source-shortcut.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1a.sh

cd /root/byod-browser
git add patches/0010-strip-view-source-shortcut.patch build/smoke-test-p1a.sh
git commit -m "patch: 0010 strip Ctrl+U view-source (patch #10a)"
```

---

## Task 5: 补丁 #10b — 移除 Save / Print 快捷键（Ctrl+S / Ctrl+P）

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/views/accelerator_table.cc`
- Create: `/root/byod-browser/patches/0011-strip-save-and-print-shortcuts.patch`

**Interfaces:**
- Consumes: Task 4 完成
- Produces: Ctrl+S、Ctrl+P（以及 mac Cmd+S / Cmd+P，若 mac 快捷键定义在同一文件里，视 Chromium 版本而定）不再绑定 IDC_SAVE_PAGE / IDC_PRINT

- [ ] **Step 1: 定位**

Run:
```bash
grep -n "IDC_SAVE_PAGE\|IDC_PRINT\|IDC_BASIC_PRINT" /opt/chromium/src/chrome/browser/ui/views/accelerator_table.cc
```

- [ ] **Step 2: 向 smoke test 追加**

Edit `/root/byod-browser/build/smoke-test-p1a.sh`:

```bash
# ---------- Patch #10b: Save/Print shortcuts removed ----------
echo "[test] patch #10b: Ctrl+S / Ctrl+P unbound"

out="$(run_chrome --headless --dump-dom chrome://accessibility 2>&1 || true)"
for id in IDC_SAVE_PAGE IDC_PRINT IDC_BASIC_PRINT; do
  if echo "$out" | grep -qi "$id"; then
    fail "#10b: $id still bound"
  else
    pass "#10b: $id not bound"
  fi
done
```

- [ ] **Step 3: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1a.sh`

- [ ] **Step 4: 注释掉相关行**

用 Edit 把包含 `IDC_SAVE_PAGE`、`IDC_PRINT`、`IDC_BASIC_PRINT` 的每一行整体注释，并在第一处上方加一行标记注释：

```cpp
    // BYOD patch #10b (spec §3.1): Save/Print accelerators removed.
```

- [ ] **Step 5: 编译 + smoke**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
```

- [ ] **Step 6: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/views/accelerator_table.cc
git commit -m "byod: strip IDC_SAVE_PAGE and IDC_PRINT accelerators

Patch #10b (spec §3.1)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0011-strip-save-and-print-shortcuts.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1a.sh

cd /root/byod-browser
git add patches/0011-strip-save-and-print-shortcuts.patch build/smoke-test-p1a.sh
git commit -m "patch: 0011 strip Ctrl+S / Ctrl+P (patch #10b)"
```

---

## Task 6: 补丁 #3 — chrome:// scheme 白名单收紧到 `chrome://exam-shell` + 必需内部页

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/webui/chrome_web_ui_controller_factory.cc`
- Modify (in Chromium src): `chrome/common/webui_url_constants.cc`
- Create: `/root/byod-browser/patches/0100-restrict-webui-scheme-to-exam-shell.patch`

**Interfaces:**
- Consumes: Task 5 完成
- Produces: 除 `chrome://exam-shell`（预留，本 sub-plan 尚未创建 WebUI，仅登记 host name） + Chromium 内部必需的少量 host（`chrome://accessibility`、`chrome://gpu`、`chrome://network-error/*` 等 —— 见白名单） 之外的所有 `chrome://` URL 都返回 404 或 blocked page

**注意**：`chrome://exam-shell` 这个 WebUI **本身**由 P6（考试壳 UI）实现；本任务只在 URL 白名单里预留它的 host 名字，避免 P6 时再回来改本补丁。

- [ ] **Step 1: 明确白名单**

**允许的 chrome:// hosts（写入补丁白名单）**：

| Host | 用途 | 出处 |
|---|---|---|
| `chrome://exam-shell` | 考试壳 UI（P6 交付） | 本项目 |
| `chrome://accessibility` | 无障碍调试；smoke test 需要 | Chromium 内建 |
| `chrome://network-error` | 网络错误页 | Chromium 内建 |
| `chrome://new-tab-page` | 新标签占位（不允许用户导航，仅内部用） | Chromium 内建 |
| `chrome://blank` | 空白页 | Chromium 内建 |
| `chrome://crash` | 崩溃页处理 | Chromium 内建 |

**禁止的**：`chrome://settings`、`chrome://flags`、`chrome://inspect`、`chrome://devtools`、`chrome://extensions`、`chrome://history`、`chrome://downloads`、以及本表未列出的一切。

- [ ] **Step 2: 定位 WebUI factory**

Run:
```bash
grep -n "GetWebUIFactoryFunction\|UseWebUIForURL" /opt/chromium/src/chrome/browser/ui/webui/chrome_web_ui_controller_factory.cc | head -10
```

- [ ] **Step 3: 追加 smoke test #3**

Edit `/root/byod-browser/build/smoke-test-p1a.sh`:

```bash
# ---------- Patch #3: chrome:// scheme whitelist ----------
echo "[test] patch #3: chrome:// hosts whitelisted"

check_url_blocked() {
  local url="$1"
  local out="$(run_chrome --headless --dump-dom "$url" 2>&1 || true)"
  if echo "$out" | grep -qi "This site can.t be reached\|ERR_BLOCKED\|not allowed\|<title>Not Found"; then
    pass "#3: $url blocked"
  elif [ -z "$out" ] || [ "$(echo "$out" | wc -c)" -lt 200 ]; then
    pass "#3: $url returns no content"
  else
    fail "#3: $url NOT blocked (returned $(echo "$out" | wc -c) chars)"
  fi
}

check_url_allowed() {
  local url="$1"
  local out="$(run_chrome --headless --dump-dom "$url" 2>&1 || true)"
  if [ "$(echo "$out" | wc -c)" -lt 100 ]; then
    fail "#3: $url unexpectedly blocked"
  else
    pass "#3: $url still accessible"
  fi
}

check_url_blocked "chrome://settings"
check_url_blocked "chrome://flags"
check_url_blocked "chrome://inspect"
check_url_blocked "chrome://extensions"
check_url_blocked "chrome://history"
check_url_blocked "chrome://downloads"

check_url_allowed "chrome://accessibility"
```

- [ ] **Step 4: 跑一次确认（未打补丁前 blocked URL 会失败）**

Run: `bash /root/byod-browser/build/smoke-test-p1a.sh`

- [ ] **Step 5: 修改 chrome_web_ui_controller_factory.cc 加白名单闸门**

在文件顶部（包含 `#include` 之后、匿名 namespace 之内）添加：

```cpp
namespace {

// BYOD patch #3 (spec §3.1): chrome:// scheme is restricted to a small,
// compile-time whitelist. Everything else returns nullptr from
// GetWebUIFactoryFunction, causing navigation to be treated as unhandled.
constexpr const char* kByodAllowedWebUIHosts[] = {
    "exam-shell",       // P6 exam shell UI
    "accessibility",    // internal a11y inspector (smoke tests)
    "network-error",    // internal error pages
    "new-tab-page",     // internal NTP (never navigated by user)
    "blank",            // internal blank page
    "crash",            // internal crash handler page
};

bool IsByodAllowedWebUIHost(const std::string& host) {
  for (const char* allowed : kByodAllowedWebUIHosts) {
    if (host == allowed) return true;
  }
  return false;
}

}  // namespace
```

然后在 `GetWebUIFactoryFunction` 函数体入口最前面加：

```cpp
  // BYOD patch #3: enforce whitelist.
  if (!IsByodAllowedWebUIHost(url.host())) {
    return nullptr;
  }
```

- [ ] **Step 6: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**若编译失败**（可能因为 `url.host()` 类型不匹配、`std::string` 未 include 等），根据 gcc/clang 报错调整；常见 fix：include `<string>`、把 `url.host()` 换成 `url.host_piece().as_string()` 或对应 API。

- [ ] **Step 7: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: 都成功

- [ ] **Step 8: 导出 patch + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/webui/chrome_web_ui_controller_factory.cc
git commit -m "byod: restrict chrome:// scheme to a compile-time whitelist

Patch #3 (spec §3.1). Only exam-shell, accessibility, network-error,
new-tab-page, blank, and crash hosts are allowed. Everything else
(settings, flags, inspect, extensions, history, downloads, ...) is
compile-time blocked."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0100-restrict-webui-scheme-to-exam-shell.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1a.sh

cd /root/byod-browser
git add patches/0100-restrict-webui-scheme-to-exam-shell.patch build/smoke-test-p1a.sh
git commit -m "patch: 0100 restrict chrome:// scheme to whitelist (patch #3)

Est. LOC: ~40. See spec §3.1."
```

---

## Task 7: 补丁 #4a — 禁 `file://` 与 `view-source:` scheme

**Files:**
- Modify (in Chromium src): `content/browser/child_process_security_policy_impl.cc` 或 `chrome/browser/browser_url_handler_impl.cc`（版本决定）
- Create: `/root/byod-browser/patches/0101-block-file-and-view-source-schemes.patch`

**Interfaces:**
- Consumes: Task 6 完成
- Produces: `file:///etc/passwd`、`view-source:https://example.com` 等 URL 全部导航失败

- [ ] **Step 1: 定位 scheme 处理入口**

Run:
```bash
grep -n "kFileScheme\|RegisterWebSafeScheme" /opt/chromium/src/content/browser/child_process_security_policy_impl.cc | head -10
grep -n "kViewSourceScheme" /opt/chromium/src/chrome/browser/browser_url_handler_impl.cc | head -10
```

- [ ] **Step 2: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1a.sh`:

```bash
# ---------- Patch #4a: file:// and view-source: blocked ----------
echo "[test] patch #4a: file:// and view-source: schemes"

# file:// — try to load /etc/passwd
out="$(run_chrome --headless --dump-dom "file:///etc/passwd" 2>&1 || true)"
if echo "$out" | grep -q "root:" ; then
  fail "#4a: file:///etc/passwd returned real /etc/passwd content"
else
  pass "#4a: file:// blocked"
fi

# view-source:
out="$(run_chrome --headless --dump-dom "view-source:https://example.com" 2>&1 || true)"
if echo "$out" | grep -q "Example Domain"; then
  # If view-source is blocked, dump-dom returns the plain example.com page or nothing.
  # If unblocked, view-source: variant would show the raw HTML wrapper. Distinguishing
  # requires looking for view-source's telltale line-number markers.
  if echo "$out" | grep -qE 'class="line-numbers"|line-number-'; then
    fail "#4a: view-source: still active"
  else
    pass "#4a: view-source: fell back to normal page"
  fi
else
  pass "#4a: view-source: blocked entirely"
fi
```

- [ ] **Step 3: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1a.sh`

- [ ] **Step 4: 修改 URL handler**

在 `/opt/chromium/src/chrome/browser/browser_url_handler_impl.cc` 里，`BrowserURLHandlerImpl::BrowserURLHandlerImpl()` 构造函数末尾，或紧接 `SetFixupHandler(...)` 之后，加入：

```cpp
  // BYOD patch #4a (spec §3.1): file:// and view-source: schemes are blocked
  // at the URL-handler layer. Navigation to these schemes is rewritten to
  // about:blank, resulting in silent block.
  AddHandlerPair(
      /* url_handler = */
      [](GURL* url, content::BrowserContext*) -> bool {
        if (url->SchemeIs(url::kFileScheme) ||
            url->SchemeIs(content::kViewSourceScheme)) {
          *url = GURL(url::kAboutBlankURL);
          return true;
        }
        return false;
      },
      /* reverse_handler = */ BrowserURLHandler::null_handler());
```

（**具体 API 形式随 Chromium 版本变化**：可能是 `AddHandlerPair(fn, null)` 也可能是 lambda 不被接受需要写成 `static bool Handler(...)` 函数。看编译错误调整。）

- [ ] **Step 5: 编译 + smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
```

- [ ] **Step 6: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/browser_url_handler_impl.cc
git commit -m "byod: block file:// and view-source: schemes

Patch #4a (spec §3.1). Rewrite to about:blank at URL-handler layer."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0101-block-file-and-view-source-schemes.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1a.sh

cd /root/byod-browser
git add patches/0101-block-file-and-view-source-schemes.patch build/smoke-test-p1a.sh
git commit -m "patch: 0101 block file:// and view-source: (patch #4a)"
```

---

## Task 8: 补丁 #4b — 禁 `chrome-devtools://` 与 `chrome-extension://` scheme

**Files:**
- Modify (in Chromium src): `chrome/browser/browser_url_handler_impl.cc`
- Create: `/root/byod-browser/patches/0102-block-chrome-devtools-and-extension-schemes.patch`

**Interfaces:**
- Consumes: Task 7 完成
- Produces: 用户可访问的 `chrome-devtools://...` 和 `chrome-extension://...` scheme 全部 rewrite 到 about:blank；Chromium 内部 CDP 使用的**非 URL 路径**不受影响

- [ ] **Step 1: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1a.sh`:

```bash
# ---------- Patch #4b: chrome-devtools:// and chrome-extension:// blocked ----------
echo "[test] patch #4b: chrome-devtools:// and chrome-extension://"

for scheme_url in "devtools://devtools/bundled/devtools_app.html" \
                  "chrome-extension://abcdefghijklmnopabcdefghijklmnop/index.html"; do
  out="$(run_chrome --headless --dump-dom "$scheme_url" 2>&1 || true)"
  # If blocked, the DOM is essentially about:blank (very short body)
  if [ "$(echo "$out" | wc -c)" -gt 500 ]; then
    fail "#4b: $scheme_url returned $(echo "$out" | wc -c) chars (not blocked)"
  else
    pass "#4b: $scheme_url blocked"
  fi
done
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1a.sh`

- [ ] **Step 3: 扩展 Task 7 里的 URL handler**

在 `chrome/browser/browser_url_handler_impl.cc` 里，找到 Task 7 加入的 lambda，扩展 scheme 列表：

```cpp
  // BYOD patch #4a+#4b (spec §3.1): block file://, view-source://,
  // devtools://, chrome-devtools://, chrome-extension:// at the URL handler.
  AddHandlerPair(
      [](GURL* url, content::BrowserContext*) -> bool {
        if (url->SchemeIs(url::kFileScheme) ||
            url->SchemeIs(content::kViewSourceScheme) ||
            url->SchemeIs(content::kChromeDevToolsScheme) ||
            url->SchemeIs("devtools") ||
            url->SchemeIs(extensions::kExtensionScheme)) {
          *url = GURL(url::kAboutBlankURL);
          return true;
        }
        return false;
      },
      BrowserURLHandler::null_handler());
```

（若 `extensions::kExtensionScheme` 引入了新头文件，加对应 `#include`；若在编译中因组件依赖不允许 include，改用字符串字面量 `"chrome-extension"`）

- [ ] **Step 4: 编译 + smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
```

- [ ] **Step 5: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/browser_url_handler_impl.cc
git commit -m "byod: block chrome-devtools:// and chrome-extension:// schemes

Patch #4b (spec §3.1). Extends #4a URL-handler filter to cover DevTools
frontend URL and extension URLs. Internal Chromium CDP dispatch remains
unaffected (uses in-process paths, not URL routing)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0102-block-chrome-devtools-and-extension-schemes.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1a.sh

cd /root/byod-browser
git add patches/0102-block-chrome-devtools-and-extension-schemes.patch build/smoke-test-p1a.sh
git commit -m "patch: 0102 block chrome-devtools:// and chrome-extension:// (patch #4b)"
```

---

## Task 9: 补丁 #11 — macOS 截屏快捷键（Cmd+Shift+3/4/5）拦截

**Files:**
- Modify (in Chromium src): `chrome/browser/app_controller_mac.mm` 或 `chrome/browser/global_keyboard_shortcuts_mac.mm`
- Create: `/root/byod-browser/patches/0500-block-macos-screenshot-shortcuts.patch`

**Interfaces:**
- Consumes: Task 8 完成
- Produces: 在 macOS 上 Chromium 主进程运行时，Cmd+Shift+3 / Cmd+Shift+4 / Cmd+Shift+5 被 Chromium 的应用级 event tap 吞掉；仍需在 macOS 系统层配合 Guardian 的 CGEventTap 才能完全拦截（Guardian 在 P4 交付）

**注意**：此补丁只在 Chromium 应用**获得焦点**时生效（Cocoa event 到达 app 才被吞）。真正需要覆盖整个 desktop 的拦截由 Guardian 做（P4）。本补丁作为**双保险的一层**。

**跨平台**：本 patch 是 mac-only。补丁文件 diff 内容包含的都是 `.mm` 文件，Linux/Windows 编译时 gn 会自动跳过；无需 `#ifdef` 包裹。

**编译验证方式**：Linux 上 apply 后 `apply-patches.sh` 成功、`build-linux.sh` 成功即可（那个 `.mm` 文件不会在 Linux 上参与编译）；行为验证要在 mac 上跑，本 sub-plan 允许**行为验证 defer 到 P0 mac 首编完成后**（记录在 acceptance checklist 里）。

- [ ] **Step 1: 定位 mac 快捷键处理**

Run:
```bash
grep -rn "NSCommandKeyMask.*NSShiftKeyMask\|kCGShieldingWindowLevel" /opt/chromium/src/chrome/browser/ | head -10
grep -n "GlobalKeyboardShortcut\|processKeyDown" /opt/chromium/src/chrome/browser/global_keyboard_shortcuts_mac.mm | head -10
```

- [ ] **Step 2: 追加 smoke test（仅在 mac 上有意义）**

Edit `/root/byod-browser/build/smoke-test-p1a.sh`:

```bash
# ---------- Patch #11: macOS screenshot shortcuts (mac only) ----------
if [[ "$(uname -s)" == "Darwin" ]]; then
  echo "[test] patch #11: Cmd+Shift+3/4/5 intercepted"
  # Behavioral test requires GUI + AppleScript, skipped in headless CI.
  # In P4 (Guardian macOS), we will test via CGEventTap fixture.
  pass "#11: (deferred to mac GUI test — see p1-a-acceptance.md)"
else
  echo "[skip] patch #11 tests (macOS only)"
fi
```

- [ ] **Step 3: 修改 mac 快捷键源文件**

在 `chrome/browser/global_keyboard_shortcuts_mac.mm` 里找到主事件处理函数（形如 `-(BOOL)handleExtraKeyboardShortcut:(NSEvent*)event`），在入口最前面添加：

```objc
  // BYOD patch #11 (spec §3.1, §4.1.5): intercept macOS screenshot shortcuts
  // when Chromium has focus. Guardian (P4) provides desktop-wide coverage via
  // CGEventTap.
  if ((event.modifierFlags & NSEventModifierFlagCommand) &&
      (event.modifierFlags & NSEventModifierFlagShift)) {
    unichar ch = [event.charactersIgnoringModifiers characterAtIndex:0];
    if (ch == '3' || ch == '4' || ch == '5') {
      return YES;  // consumed
    }
  }
```

- [ ] **Step 4: Linux 上仅验证 apply/编译（.mm 不参与 Linux 编译）**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/global_keyboard_shortcuts_mac.mm
git commit -m "byod: intercept macOS screenshot shortcuts (Cmd+Shift+3/4/5)

Patch #11 (spec §3.1). Application-level fallback; desktop-wide coverage
by Guardian in P4."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0500-block-macos-screenshot-shortcuts.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: apply 与 build 都成功；smoke test 打印 `[skip] patch #11 tests (macOS only)`

- [ ] **Step 5: Commit**

Run:
```bash
cd /root/byod-browser
git add patches/0500-block-macos-screenshot-shortcuts.patch build/smoke-test-p1a.sh
git commit -m "patch: 0500 intercept macOS screenshot shortcuts (patch #11)

Behavioral verification deferred to macOS build (documented in
p1-a-acceptance.md)."
```

---

## Task 10: 补丁总量预算校验 + 交付确认

**Files:**
- Create: `/root/byod-browser/docs/development/p1-a-acceptance.md`

**Interfaces:**
- Consumes: Task 1..9 全部完成
- Produces: P1-A 交付验收 checklist；预算数据快照

- [ ] **Step 1: 统计当前 patch 总行数**

Run:
```bash
source /root/byod-browser/build/chromium-version.env
cd /opt/chromium/src
git diff --shortstat "$CHROMIUM_TAG"..HEAD
```
Expected: 形如 `9 files changed, ~400 insertions(+), ~30 deletions(-)`；insertions 应 ≤ 600（本批预算）

**若超预算**：回到最"重"的补丁（大概率是 #3 chrome:// 白名单），review 是否有可精简的部分。

- [ ] **Step 2: 手动跑一遍 CI lint 脚本对齐**

Run:
```bash
cd /root/byod-browser
bash -c 'total=0; for p in patches/*.patch; do added=$(grep -c "^+" "$p" || true); total=$((total + added)); done; echo "grep-based total added lines: $total"'
```
Expected: 输出 <600（若 >600 会在 CI 报 warn，暂不 fail 因为 grep-based 计数含 metadata）

- [ ] **Step 3: 验证所有 patch 都能干净应用**

Run:
```bash
echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
```
Expected: `[success] all patches applied on top of <TAG>`；无 conflict

- [ ] **Step 4: 全量编译 + 全量 smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
```
Expected: 全部 pass

- [ ] **Step 5: 写 acceptance checklist**

Write `/root/byod-browser/docs/development/p1-a-acceptance.md`:

```markdown
# P1-A 交付验收 Checklist

## 补丁类

- [ ] `patches/0001-disable-devtools-window-entrypoints.patch` 存在
- [ ] `patches/0002-noop-open-devtools-shortcut.patch` 存在
- [ ] `patches/0003-strip-remote-debugging-switches.patch` 存在
- [ ] `patches/0010-strip-view-source-shortcut.patch` 存在
- [ ] `patches/0011-strip-save-and-print-shortcuts.patch` 存在
- [ ] `patches/0100-restrict-webui-scheme-to-exam-shell.patch` 存在
- [ ] `patches/0101-block-file-and-view-source-schemes.patch` 存在
- [ ] `patches/0102-block-chrome-devtools-and-extension-schemes.patch` 存在
- [ ] `patches/0500-block-macos-screenshot-shortcuts.patch` 存在

## 编译类

- [ ] `bash build/revert-patches.sh && bash build/apply-patches.sh` 无冲突
- [ ] `bash build/build-linux.sh chrome` 成功
- [ ] `git diff --shortstat "$CHROMIUM_TAG"..HEAD` 显示 insertions ≤ 600

## 行为类（Linux headless smoke tests）

- [ ] `bash build/smoke-test.sh` 全部 pass
- [ ] `bash build/smoke-test-p1a.sh` 全部 pass（含 patch #11 在 Linux 上 skip）

## 行为类（macOS，若 mac 首编已完成）

- [ ] 在 mac 上手动打开一个页面，按 Cmd+Shift+3 → 不产生截屏文件
- [ ] 在 mac 上手动打开一个页面，按 Cmd+Shift+4 → 不进入选区截屏模式
- [ ] 在 mac 上手动打开一个页面，按 Cmd+Shift+5 → 不弹出截屏工具栏
- [ ] （defer 项）patch #11 在 macOS 端的完整拦截由 P4 Guardian 完成，本批只做 app 焦点内的一层拦截

## 行为类（Windows，若 Windows 首编已完成）

- [ ] `powershell -File build\build-windows.ps1 -Target chrome` 成功
- [ ] 手动运行，Ctrl+U 不打开 view-source
- [ ] 手动运行，Ctrl+S 不弹保存对话框
- [ ] 手动运行，Ctrl+P 不弹打印对话框
- [ ] 手动运行，F12 / Ctrl+Shift+I 不打开 DevTools

## 文档类

- [ ] 本文件（p1-a-acceptance.md）存在
- [ ] `docs/superpowers/plans/2026-07-11-byod-roadmap.md` 里 P1 标记从"待写"移动到"进行中"（本批完成后仍是"进行中"，直到 P1-B..P1-E 都完成才 checkmark 为"完成"）

## 交付判定

以上全部打勾（macOS/Windows 项若资源到位）→ P1-A 完成，可进入 [P1-B](../superpowers/plans/2026-07-12-byod-p1-b-window-navigation.md)（尚未编写）。
```

- [ ] **Step 6: Commit acceptance**

Run:
```bash
cd /root/byod-browser
git add docs/development/p1-a-acceptance.md
git commit -m "docs: add P1-A acceptance checklist"
```

- [ ] **Step 7: 更新 roadmap 标 P1 状态为"进行中"**

Edit `/root/byod-browser/docs/superpowers/plans/2026-07-11-byod-roadmap.md`，把 P1 那一行的 `📝 待写` 改成 `⏳ 进行中 (P1-A 完成)`。

- [ ] **Step 8: Commit**

Run:
```bash
cd /root/byod-browser
git add docs/superpowers/plans/2026-07-11-byod-roadmap.md
git commit -m "docs: mark P1 in-progress (P1-A complete)"
```

---

## P1-A 完成后

- 已交付 9 个补丁文件（6 个 spec 编号的补丁 = #1a+#1b、#2、#3、#4a+#4b、#10a+#10b、#11）
- 累计 patch 行数应 ≤ 600（本批预算）；spec §3.1 总预算 3000
- 下一批：**P1-B**（补丁 #5 window.open、#6 菜单裁剪、#7 Kiosk 强制），预算 ~580 行

**预计实际耗时**（含每次增量编译）：
- 熟练 Chromium 开发者：3-5 天
- 首次改 Chromium 的开发者：1-2 周（含碰到 API 差异的调试时间）

**风险与常见坑：**

- **API 版本差异**：本文档以 Chromium 130 为参照，实际执行时行号 / 函数签名可能已变。Task 内所有 grep 步骤都是"先定位再改"，就是对付这个的
- **编译报错**：改 `content/` 层要小心 `component build` 的 export macro；找不到符号先查 `BUILD.gn`
- **URL handler 顺序**：`AddHandlerPair` 是顺序注册的，本批的 handler 应尽早注册（在 default handler 前），否则可能被别的 handler 提前 rewrite 导致 bypass
- **Chromium 内部 CDP 依赖**：如果打完 patch 后 print preview / reader mode / accessibility 挂了，说明动到了内部通信路径，需 revert 后细化到只删用户可见入口

**若整批耗时超过 2 周**：暂停，回头 review 是否应把某个补丁移到 P1-B / P1-C 或 defer 到 P2 后。
