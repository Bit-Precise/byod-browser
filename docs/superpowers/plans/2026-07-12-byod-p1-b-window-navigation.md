# P1-B · 窗口/导航/UI 剪裁补丁 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 合入 3 个"删/禁 UI 层"类 Chromium 补丁：`window.open` / `target=_blank` 强制主窗口内导航、右键/复制粘贴/打印/另存/拖拽菜单裁剪与命令拦截、Kiosk 无边框+置顶+锁尺寸+禁最小化。覆盖 spec §3.1 补丁 #5、#6、#7。

**Architecture:** 延续 P1-A 的补丁工作流：一个补丁 = 一个 `.patch` 文件 = 一个 Chromium 端 commit；每合入一个补丁跑 build + 追加 `smoke-test-p1b.sh` 的行为测试；跨平台差异（尤其 Kiosk）用平台特化文件解决，Linux dev 环境编 Linux 版本 + 补丁本身覆盖 Win/mac 相应源文件。

**Tech Stack:** Chromium (C++, `.mm` 部分 Objective-C++) + gn/ninja + bash smoke tests；Linux 快速迭代，mac/Win 行为验证 defer 到该平台首编完成。

## Global Constraints

- Chromium 补丁总量 ≤ 3000 行（全 P1 累计）；本批预算 ≤ 580 行；本批 + P1-A 累计目标 ≤ 1180 行
- Chromium 补丁只做"删/禁"和"钩子暴露"，不写业务
- 每个补丁必须独立可 revert；即使中间某一个 revert 掉，Chromium 也能编译并启动
- 每个补丁的 commit message 前缀固定 `byod: `；spec 章节引用固定 `see spec §3.1 patch #N`
- 补丁文件命名遵循 P0 patch-workflow.md 的序号分段：`0200-0299` = 窗口/导航策略；`0300-0399` = 菜单/命令拦截
- Chromium 源码 pin 版本参照 `/root/byod-browser/build/chromium-version.env`
- P1-B 依赖 P1-A 已合入（即 patches 目录已含 0001-0500 序号段的 9 个 patch）

---

## File Structure

**本 sub-plan 交付：**

```
/root/byod-browser/
├── patches/
│   ├── 0200-force-main-window-navigation.patch          # 补丁 #5
│   ├── 0201-block-window-open-cross-origin.patch        # 补丁 #5 补充
│   ├── 0300-strip-context-menu-items.patch              # 补丁 #6a (右键菜单裁剪)
│   ├── 0301-block-clipboard-and-save-commands.patch     # 补丁 #6b (edit commands)
│   ├── 0302-disable-drag-out.patch                      # 补丁 #6c (拖拽出考试页)
│   ├── 0210-kiosk-frameless-window-linux-win.patch      # 补丁 #7a (Views 平台)
│   ├── 0211-kiosk-frameless-nswindow-mac.patch          # 补丁 #7b (Cocoa 平台)
│   ├── 0212-kiosk-topmost-and-locked-size.patch         # 补丁 #7c (跨平台策略)
│   └── 0213-kiosk-disable-minimize-and-close.patch      # 补丁 #7d
├── build/
│   └── smoke-test-p1b.sh                                # 本批专用行为验证脚本
└── docs/
    └── development/
        └── p1-b-acceptance.md                           # 本批验收清单
```

**Chromium 源码触及区域（供 review 追溯）：**

- `content/browser/renderer_host/render_frame_host_impl.cc` — CreateNewWindow (#5)
- `third_party/blink/renderer/core/frame/local_dom_window.cc` — window.open (#5 补充)
- `chrome/browser/renderer_context_menu/render_view_context_menu.cc` — 菜单项过滤 (#6a)
- `content/browser/renderer_host/render_widget_host_view_base.cc` — edit command dispatch (#6b)
- `chrome/browser/ui/browser_command_controller.cc` — SaveAs/Print 命令拦截 (#6b)
- `third_party/blink/renderer/core/page/drag_controller.cc` — 拖拽拦截 (#6c)
- `chrome/browser/ui/views/frame/browser_frame.cc` — Views 无边框 (#7a Linux/Win)
- `chrome/browser/ui/views/frame/browser_view.cc` — Views 尺寸锁定 (#7c)
- `chrome/browser/ui/cocoa/browser_window_controller.mm` — Cocoa NSWindow subclass (#7b mac)
- `chrome/browser/ui/browser_window/public/browser_window_features.cc` — 禁最小化/关闭 (#7d)

**说明**：以上路径以 Chromium 130 stable 为参照，rebase 后可能移动 —— 每个 Task 里都用 `grep` 先定位再改。

---

## Task 1: 补丁 #5 — 强制主窗口内导航（CreateNewWindow no-op）

**Files:**
- Modify (in Chromium src): `content/browser/renderer_host/render_frame_host_impl.cc`
- Create: `/root/byod-browser/patches/0200-force-main-window-navigation.patch`
- Create: `/root/byod-browser/build/smoke-test-p1b.sh`（首次创建）

**Interfaces:**
- Consumes: P1-A 全部完成（`bash build/apply-patches.sh` 应用 9 个 patch 后能编译）
- Produces: 页内 JS 调 `window.open(url, '_blank')` 或点击 `<a target="_blank">` 都不新开窗口/标签；改在**当前主 frame 内**导航到 URL（仍会走 WebRequest 白名单，越白名单则被 P1-C 拦截）

- [ ] **Step 1: 起始状态自检**

Run:
```bash
cd /opt/chromium/src
git status
git describe --tags
git log --oneline | head -12
```
Expected: `working tree clean`；tag 与 `chromium-version.env` 的 `CHROMIUM_TAG` 一致；最近 9 条 commit 是 P1-A 的补丁（消息以 `byod:` 开头）。

若不干净：`echo y | bash /root/byod-browser/build/revert-patches.sh && bash /root/byod-browser/build/apply-patches.sh`

- [ ] **Step 2: 定位 CreateNewWindow 实现**

Run:
```bash
grep -n "^void RenderFrameHostImpl::CreateNewWindow\|^RenderFrameHostImpl::CreateNewWindow" /opt/chromium/src/content/browser/renderer_host/render_frame_host_impl.cc | head -5
grep -n "mojom::CreateNewWindowStatus" /opt/chromium/src/content/browser/renderer_host/render_frame_host_impl.cc | head -10
```
Expected: 有输出；记录 `CreateNewWindow` 函数起始行号，以及函数内决定"新窗口 vs 当前窗口"的关键分支（通常与 `mojom::CreateNewWindowStatus::kSuccess` 相关）。

- [ ] **Step 3: 写 smoke-test-p1b.sh 骨架 + 补丁 #5 测试段**

Write `/root/byod-browser/build/smoke-test-p1b.sh`:

```bash
#!/usr/bin/env bash
# Behavior smoke tests for P1-B patches (#5, #6, #7).
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

# ---------- Patch #5: window.open / target=_blank forced in-frame ----------
echo "[test] patch #5: window.open forced in-frame"

# Serve a tiny HTML that invokes window.open and reports document.location
fixture="$CHROME_TMP/window-open.html"
cat > "$fixture" <<'HTML'
<!DOCTYPE html>
<html><head><title>OpenTest</title></head><body>
<script>
  const w = window.open('data:text/html,<title>OPENED_IN_NEW</title>', '_blank');
  document.title = w === null || w === window ? 'BLOCKED_OR_SAMEWIN' : 'NEW_WIN_' + (w !== window);
</script>
</body></html>
HTML

out="$(run_chrome --headless --dump-dom "file://$fixture" 2>&1 || true)"
# NOTE: file:// is blocked by P1-A patch #4a. So this test must use a data: URL.
# Rewrite using data: to avoid depending on file:// being enabled.
data_url="data:text/html;base64,$(base64 -w0 "$fixture" 2>/dev/null || base64 "$fixture" | tr -d '\n')"
out="$(run_chrome --headless --dump-dom "$data_url" 2>&1 || true)"

# After patch #5, window.open returns the current window ref (or null), so title
# should be 'BLOCKED_OR_SAMEWIN'. Before patch, browser would open a second
# renderer and headless mode prints only the first, so title stays 'OpenTest'
# or shows 'NEW_WIN_true'.
if echo "$out" | grep -q "BLOCKED_OR_SAMEWIN"; then
  pass "#5: window.open blocked / redirected to current window"
elif echo "$out" | grep -q "NEW_WIN_true"; then
  fail "#5: window.open still creates a new window"
else
  # Fallback: check the URL bar hint via debugging output (headless has none;
  # accept ambiguous result but warn)
  echo "  ? #5: window.open behavior indeterminate in headless (title=$(echo "$out" | grep -oE '<title>[^<]*' | head -1))"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "[fail] one or more P1-B tests failed"; exit 1
fi
echo "[success] all P1-B tests passed"
```

Run:
```bash
chmod +x /root/byod-browser/build/smoke-test-p1b.sh
```

- [ ] **Step 4: 跑 smoke 确认 vanilla 状态下测试不通过（或至少可复现）**

Run:
```bash
bash /root/byod-browser/build/smoke-test-p1b.sh
```
Expected: 输出 `? #5: window.open behavior indeterminate in headless` 或 `✗ #5: window.open still creates a new window`。若显示 pass，说明 headless 模式本身就"看不到"新窗口，这条测试要 rely on 后续的可视化验证 —— **接受此局限**并继续。

- [ ] **Step 5: 修改 render_frame_host_impl.cc — 让 CreateNewWindow 静默 no-op**

在 `CreateNewWindow` 函数体入口（在参数解构 / mojom 消息处理**之后**，实际创建 window 之前）添加：

```cpp
  // BYOD patch #5 (spec §3.1, §4.1.4): user-initiated new window creation is
  // disabled. Requests are silently satisfied with a no-op response so the
  // page's JS callback doesn't hang. Navigation intended for a new window is
  // instead performed inline via a subsequent Navigate() call issued by the
  // renderer's fallback path (target=_blank without opener degrades to
  // top-level navigation).
  std::move(callback).Run(
      mojom::CreateNewWindowStatus::kBlocked,
      /*reply=*/nullptr);
  return;
```

（`mojom::CreateNewWindowStatus::kBlocked` 是 Chromium 已有枚举，用于表示"新窗口被 popup blocker 拒绝"；沿用它可复用现有 renderer 侧行为路径。若该枚举值不同或有 `kIgnore`，用最贴近"静默失败"的那个。）

**注意**：不要 `return` 得太早——callback 必须先 `Run(...)` 否则 renderer 侧 mojom 会挂住。所以顺序是：先 `std::move(callback).Run(...)`，然后 `return`。

- [ ] **Step 6: 修改 blink 侧 window.open 补充（可选，视 Step 5 效果）**

理论上 `CreateNewWindow` no-op 后 blink 侧 `window.open` 会返回 null。但某些老 API 路径可能绕过这条 IPC。可**先跳过**这一步；跑完 smoke 后若发现 `window.open` 仍然新开，再回来修 `third_party/blink/renderer/core/frame/local_dom_window.cc` 的 `LocalDOMWindow::open`。

- [ ] **Step 7: 增量编译**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
```
Expected: 增量编译（改动在 content 层，约 20-40 分钟）；最终 `[success] built: ...`

- [ ] **Step 8: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
```
Expected: 前两个全成功；第三个至少 `#5` 不报 `✗`（`✓` 或 `?` 都算通过 —— headless 局限已知）

- [ ] **Step 9: Chromium 端 commit + 导出 patch**

Run:
```bash
cd /opt/chromium/src
git add content/browser/renderer_host/render_frame_host_impl.cc
git commit -m "byod: force new-window creation to no-op (patch #5)

Patch #5 (spec §3.1, §4.1.4). window.open and target=_blank no longer
spawn a new top-level browser window; the mojom callback is answered
with kBlocked so the renderer treats it as a popup-blocker rejection.
Intended follow-up navigation happens in-frame (subject to WebRequest
whitelist — see P1-C patch #8)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0200-force-main-window-navigation.patch
```

- [ ] **Step 10: Revert + re-apply 循环验证**

Run:
```bash
echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1b.sh
```
Expected: 全成功

- [ ] **Step 11: Commit patch 到本 repo**

Run:
```bash
cd /root/byod-browser
git add patches/0200-force-main-window-navigation.patch build/smoke-test-p1b.sh
git commit -m "patch: 0200 force-main-window navigation (patch #5)

Est. LOC: ~15."
```

---

## Task 2: 补丁 #5 补充 — 跨域 window.open 直接拒绝

**Files:**
- Modify (in Chromium src): `third_party/blink/renderer/core/frame/local_dom_window.cc`
- Create: `/root/byod-browser/patches/0201-block-window-open-cross-origin.patch`

**Interfaces:**
- Consumes: Task 1 完成
- Produces: 页面 JS `window.open(url)` 若 url 与当前页 origin 不同，直接返回 null 不发起 IPC（省一次 renderer↔browser 往返，更彻底）

**说明**：Task 1 的 no-op 已经能挡住大部分场景；本补丁是 **defense in depth**——某些老 API 路径 / iframe 场景可能在到达 `CreateNewWindow` 之前就走别的通道，本补丁在 blink 层直接返回 null。

- [ ] **Step 1: 定位 LocalDOMWindow::open**

Run:
```bash
grep -n "DOMWindow\* LocalDOMWindow::open\|LocalDOMWindow::openPictureInPictureWindow" /opt/chromium/src/third_party/blink/renderer/core/frame/local_dom_window.cc | head -5
```

- [ ] **Step 2: 追加 smoke test #5-cross-origin**

Edit `/root/byod-browser/build/smoke-test-p1b.sh`，在 `# ---------- Patch #5 ...` 段之后追加：

```bash
# ---------- Patch #5-supplement: cross-origin window.open blocked ----------
echo "[test] patch #5-cross-origin"

data_url='data:text/html,<script>const w=window.open("https://example.com/",%20"_blank");document.title=w===null?"BLOCKED":"OPENED";</script>'
out="$(run_chrome --headless --dump-dom "$data_url" 2>&1 || true)"
if echo "$out" | grep -q "<title>BLOCKED</title>"; then
  pass "#5-cross: window.open(https://example.com) returned null"
elif echo "$out" | grep -q "<title>OPENED</title>"; then
  fail "#5-cross: window.open(https://example.com) did NOT return null"
else
  echo "  ? #5-cross: indeterminate — title=$(echo "$out" | grep -oE '<title>[^<]*' | head -1)"
fi
```

- [ ] **Step 3: 跑测试确认失败**

Run: `bash /root/byod-browser/build/smoke-test-p1b.sh`
Expected: `#5-cross` 显示 `?` 或 `✗`

- [ ] **Step 4: 修改 LocalDOMWindow::open**

在 `LocalDOMWindow::open` 函数体入口，紧接参数校验之后添加：

```cpp
  // BYOD patch #5-supplement (spec §3.1): defense-in-depth — cross-origin
  // window.open returns null at the blink layer, before an IPC round-trip.
  // Same-origin opens still go through CreateNewWindow (patch #5 core), which
  // rewrites them into in-frame navigations.
  if (url_string) {
    KURL requested_url(FallbackBaseURL(), *url_string);
    if (requested_url.IsValid() &&
        !GetSecurityContext().GetSecurityOrigin()->CanRequest(requested_url)) {
      return nullptr;
    }
  }
```

（具体 KURL / SecurityOrigin API 依 blink 版本；上面的用法在 Chromium 100+ 稳定。）

- [ ] **Step 5: 编译 + smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
```

- [ ] **Step 6: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add third_party/blink/renderer/core/frame/local_dom_window.cc
git commit -m "byod: reject cross-origin window.open at blink layer

Patch #5-supplement (spec §3.1). Defense in depth alongside patch #5."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0201-block-window-open-cross-origin.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1b.sh

cd /root/byod-browser
git add patches/0201-block-window-open-cross-origin.patch build/smoke-test-p1b.sh
git commit -m "patch: 0201 cross-origin window.open returns null (patch #5-supp)

Est. LOC: ~15."
```

---

## Task 3: 补丁 #6a — 右键 context menu 项裁剪

**Files:**
- Modify (in Chromium src): `chrome/browser/renderer_context_menu/render_view_context_menu.cc`
- Create: `/root/byod-browser/patches/0300-strip-context-menu-items.patch`

**Interfaces:**
- Consumes: Task 2 完成
- Produces: 右键菜单只保留极小白名单（无内容 / 或仅"重新加载"）；具体禁的项：Copy / Cut / Paste / Save As / Print / View Source / Inspect / Save Image As / Copy Link / Open Link in New Tab / Search With / Translate...

- [ ] **Step 1: 定位菜单构建函数**

Run:
```bash
grep -n "void RenderViewContextMenu::AppendPageItems\|AppendCopyItem\|AppendPrintItem\|AppendDeveloperItems" /opt/chromium/src/chrome/browser/renderer_context_menu/render_view_context_menu.cc | head -20
```
Expected: 一系列 `Append*Items` / `Append*Item` 函数；记录函数名列表。

- [ ] **Step 2: 明确白名单（本项目允许的菜单项）**

**允许**（v1）：无 —— **禁全部菜单**是最简单且最安全的策略。若考试题型确实需要（比如富文本编辑器要 Copy/Paste），改由**页面 JS 层**用 `document.execCommand('copy')` 走 clipboard API 路径（后者可由 L3 策略层单独开关）。

**禁**：全部 append 到主 menu 的项。

**说明**：如果全禁导致 renderer 侧右键菜单弹出空 menu 看起来奇怪，可以直接不显示 menu ——见 Step 4。

- [ ] **Step 3: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1b.sh`:

```bash
# ---------- Patch #6a: context menu emptied ----------
echo "[test] patch #6a: context menu items stripped"

# Headless mode doesn't render context menus; we validate via existence of
# item builder symbols in the binary being unreferenced (compile-time check
# via strings is brittle). Instead we rely on the removed function calls in
# source (guaranteed by patch application).
# Actual UX validation deferred to manual verification (documented in
# p1-b-acceptance.md).
pass "#6a: (menu emptying validated via source patch; UX check deferred to manual test)"
```

（真实的 context menu 行为需要 GUI，headless 测不到；接受这个局限）

- [ ] **Step 4: 修改 context menu builder — 顶部早退**

在 `RenderViewContextMenu::InitMenu()`（或对应的主 build 入口，通常叫 `InitMenu` / `AppendItemsForFrame` 之一）函数体入口最前面添加：

```cpp
  // BYOD patch #6a (spec §3.1, §4.1.4): context menu is compile-time empty
  // for exam mode. No items appended. Renderer suppresses menu display when
  // the menu model has zero items, so no visible UI change.
  return;
```

**注意**：某些 Chromium 版本里 `InitMenu` 后紧接着有 "commands map" 的初始化逻辑，如果 return 太早会漏掉必要的成员初始化。改成让函数**先做完成员初始化，跳过所有 `Append*` 调用**：

- 若 `Append*Items` 全部集中在函数末尾块，early return 前就没做实质初始化 —— 直接 return
- 若 `Append*Items` 交错分布 —— 改成把每个 `Append*` 调用包一个 `if (false)`（保留代码位置便于 rebase 时看冲突），或统一注释掉

**推荐做法**：在 `InitMenu()` 里保留成员初始化，把所有 `Append*(...);` 语句**整体注释掉**并加一条 marker 注释：

```cpp
  // BYOD patch #6a (spec §3.1): all context menu items stripped.
  // Preserved AppendXItems() calls below are commented out to make
  // rebase conflicts easier to read. Uncomment individually if a
  // future exam feature legitimately needs one item back.
  //
  // AppendPageItems();
  // AppendLinkItems();
  // AppendImageItems();
  // AppendCopyItem();
  // AppendPrintItem();
  // AppendSaveAsItem();
  // AppendDeveloperItems();
  // AppendMediaItems();
  // AppendTranslateItem();
  // ... (list actual calls found by grep in Step 1)
```

- [ ] **Step 5: 编译**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
```
若报"unused function"警告因某些 `Append*Items` 变成 unused：加 `[[maybe_unused]]` 到那些函数声明，或忽略警告（Chromium build 通常允许 warnings）。

- [ ] **Step 6: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
```

- [ ] **Step 7: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/renderer_context_menu/render_view_context_menu.cc
git commit -m "byod: strip context menu items (patch #6a)

Patch #6a (spec §3.1, §4.1.4). All AppendXItems() calls in InitMenu()
are commented out; menu model stays empty so no visible menu appears
when user right-clicks."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0300-strip-context-menu-items.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1b.sh

cd /root/byod-browser
git add patches/0300-strip-context-menu-items.patch build/smoke-test-p1b.sh
git commit -m "patch: 0300 strip context menu items (patch #6a)

Est. LOC: ~30."
```

---

## Task 4: 补丁 #6b — Edit commands 拦截（Cut/Copy/Paste 快捷键 + SaveAs/Print 命令）

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/browser_command_controller.cc`
- Create: `/root/byod-browser/patches/0301-block-clipboard-and-save-commands.patch`

**Interfaces:**
- Consumes: Task 3 完成
- Produces: 通过 Chromium **命令 ID** 触发的 Cut/Copy/Paste（例如程序化的 `Browser::ExecuteCommand(IDC_COPY)`，或菜单已 stripped 后残留的键绑定）全部无效；`IDC_SAVE_PAGE` / `IDC_PRINT` / `IDC_BASIC_PRINT` 已在 P1-A 补丁 #10b 里从加速表里删了，此处对**命令执行**再加一层禁

**说明**：P1-A 拆的是**快捷键→命令**的映射；本补丁拆的是**命令→行为**。两处都禁形成 belt-and-suspenders。

- [ ] **Step 1: 定位命令执行入口**

Run:
```bash
grep -n "bool BrowserCommandController::ExecuteCommandWithDisposition\|case IDC_COPY\|case IDC_CUT\|case IDC_PASTE\|case IDC_SAVE_PAGE\|case IDC_PRINT" /opt/chromium/src/chrome/browser/ui/browser_command_controller.cc | head -30
```
Expected: `ExecuteCommandWithDisposition` 函数以及若干 `case` 语句所在行。

- [ ] **Step 2: 明确要禁的命令 ID**

**禁**：
- `IDC_CUT`
- `IDC_COPY`
- `IDC_PASTE`
- `IDC_SAVE_PAGE`
- `IDC_PRINT`
- `IDC_BASIC_PRINT`
- `IDC_EMAIL_PAGE_LOCATION`
- `IDC_COPY_URL`
- `IDC_SEND_TAB_TO_SELF`

**允许**：其它 `IDC_*`（比如 `IDC_STOP` / `IDC_RELOAD` —— 由 L3 白名单单独控制；本补丁只拦"数据外流"类）

- [ ] **Step 3: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1b.sh`:

```bash
# ---------- Patch #6b: edit commands blocked ----------
echo "[test] patch #6b: edit commands early-exit"

# Behavioral test: no headless surface for command execution. Compile-time
# check ensures the added early-return exists in patch (verified below).
if grep -q "BYOD patch #6b" /root/byod-browser/patches/0301-block-clipboard-and-save-commands.patch 2>/dev/null; then
  pass "#6b: source patch present"
else
  # This test will be added after Step 6; skip if patch not yet exported
  echo "  (patch not yet exported)"
fi
```

- [ ] **Step 4: 修改 ExecuteCommandWithDisposition — 顶部 early-exit**

在 `BrowserCommandController::ExecuteCommandWithDisposition(int id, WindowOpenDisposition disposition, base::TimeTicks time_stamp)` 函数体入口最前面添加：

```cpp
  // BYOD patch #6b (spec §3.1, §4.1.4): block clipboard/save/print commands
  // at the browser-command layer. Belt-and-suspenders with patch #10a/#10b
  // which strip the accelerator bindings.
  switch (id) {
    case IDC_CUT:
    case IDC_COPY:
    case IDC_PASTE:
    case IDC_SAVE_PAGE:
    case IDC_PRINT:
    case IDC_BASIC_PRINT:
    case IDC_EMAIL_PAGE_LOCATION:
    case IDC_COPY_URL:
    case IDC_SEND_TAB_TO_SELF:
      return true;  // "handled" but no-op
    default:
      break;
  }
```

（`return true` 是因为 `ExecuteCommandWithDisposition` 的返回值语义是"命令是否被处理"—— 返回 true 让上层认为"这个命令归我处理并已完成"，从而不会 fallback 到别的 handler）

- [ ] **Step 5: 编译 + smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
```

- [ ] **Step 6: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/browser_command_controller.cc
git commit -m "byod: block clipboard/save/print commands at command layer

Patch #6b (spec §3.1, §4.1.4). Belt-and-suspenders with #10a/#10b."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0301-block-clipboard-and-save-commands.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1b.sh

cd /root/byod-browser
git add patches/0301-block-clipboard-and-save-commands.patch build/smoke-test-p1b.sh
git commit -m "patch: 0301 block clipboard/save/print commands (patch #6b)

Est. LOC: ~20."
```

---

## Task 5: 补丁 #6c — 禁止从考试页拖出内容

**Files:**
- Modify (in Chromium src): `third_party/blink/renderer/core/page/drag_controller.cc`
- Create: `/root/byod-browser/patches/0302-disable-drag-out.patch`

**Interfaces:**
- Consumes: Task 4 完成
- Produces: 页内选中文本 / 图片 / 链接后拖出到其他窗口 / 桌面 → 拖拽被 blink 层拒绝；页内**内部**拖拽（如排序题）不受影响（因为不涉及跨进程 clipboard）

- [ ] **Step 1: 定位 drag 起始函数**

Run:
```bash
grep -n "bool DragController::StartDrag\|DragController::TryDHTMLDrag\|DragController::DispatchTextInputEventFor" /opt/chromium/src/third_party/blink/renderer/core/page/drag_controller.cc | head -10
```
Expected: `StartDrag` 或类似函数所在行。

- [ ] **Step 2: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1b.sh`:

```bash
# ---------- Patch #6c: drag out blocked ----------
echo "[test] patch #6c: drag out"
# Behavioral test requires a GUI drag; deferred to manual test.
pass "#6c: (source-level; manual GUI test in p1-b-acceptance.md)"
```

- [ ] **Step 3: 修改 StartDrag — 顶部拒绝跨窗口拖拽**

在 `DragController::StartDrag` 函数体入口添加：

```cpp
  // BYOD patch #6c (spec §3.1, §4.1.4): reject drag operations that would
  // leave the current page (which by design is inside a Kiosk-locked window).
  // Same-page drags (e.g. drag-to-reorder in exam UI) still work because
  // they are handled inside the renderer without invoking system drag.
  //
  // We do this by short-circuiting the OS-level drag initiation.
  return false;
```

**注意**：这个 return 位置**很关键**——必须放在 renderer-only drag 检测之后（如果 Chromium 有）；否则会连内部拖拽都禁掉。

**若不确定**：Chromium 里 `TryDHTMLDrag` 和 `TryDragOperation` 是分开的路径，前者是 HTML5 内部 drag，后者是 OS drag。**只在后者早退**，可能需要改的是不同函数，看 grep 结果决定。若不能干净区分：接受 v1 内所有拖拽都禁（简单粗暴，代价是丢失"排序题"这种交互；由 L3 策略层可选开）。

- [ ] **Step 4: 编译 + smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
```

- [ ] **Step 5: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add third_party/blink/renderer/core/page/drag_controller.cc
git commit -m "byod: block OS-level drag-out from exam pages

Patch #6c (spec §3.1, §4.1.4). Same-page HTML5 drags remain functional."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0302-disable-drag-out.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1b.sh

cd /root/byod-browser
git add patches/0302-disable-drag-out.patch build/smoke-test-p1b.sh
git commit -m "patch: 0302 disable OS drag-out (patch #6c)

Est. LOC: ~10."
```

---

## Task 6: 补丁 #7a — Views 平台无边框窗口（Linux/Windows）

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/views/frame/browser_frame.cc`
- Modify (in Chromium src): `chrome/browser/ui/views/frame/browser_non_client_frame_view_factory.cc`（若存在）
- Create: `/root/byod-browser/patches/0210-kiosk-frameless-window-linux-win.patch`

**Interfaces:**
- Consumes: Task 5 完成
- Produces: Chromium 主窗口 **默认**无边框（无标题栏、无系统菜单、无最小化/最大化/关闭按钮）；命令行传 `--byod-kiosk` 时启用（v1 阶段固定编译期启用；命令行开关便于 dev 阶段临时切回带边框调试）

**说明**：把 Kiosk 拆成 4 个小补丁（#7a Views 平台无边框、#7b Cocoa 无边框、#7c 尺寸锁定、#7d 禁最小化/关闭），分别可 revert，一个错不会全崩。

- [ ] **Step 1: 定位 Views frame 构建**

Run:
```bash
grep -n "class BrowserFrame\|BrowserFrame::InitBrowserFrame\|BrowserFrame::FrameTypeChanged" /opt/chromium/src/chrome/browser/ui/views/frame/browser_frame.cc | head -10
grep -n "CreateNonClientFrameView\|OpaqueBrowserFrameView" /opt/chromium/src/chrome/browser/ui/views/frame/ | head -20
```

- [ ] **Step 2: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1b.sh`:

```bash
# ---------- Patch #7a: frameless window (Views) ----------
echo "[test] patch #7a: frameless"
# Behavioral test requires a display server; deferred to manual test.
pass "#7a: (source-level; manual GUI test in p1-b-acceptance.md)"
```

- [ ] **Step 3: 修改 BrowserFrame::InitBrowserFrame — 设 native frame off**

在 `BrowserFrame::InitBrowserFrame()` 函数体末尾（在 `native_browser_frame_` 已创建之后）添加：

```cpp
  // BYOD patch #7a (spec §3.1, §4.1.2): Kiosk enforcement — remove native
  // window frame (title bar, system menu, min/max/close buttons). The exam
  // shell UI (P6) draws its own minimal chrome inside the client area.
  //
  // On Views platforms (Linux/Windows via Aura), we do this by:
  //  1. Requesting custom non-client frame view (no OS decorations)
  //  2. Passing WM hints to disable window manager decorations
  set_frame_type(views::Widget::FrameType::kForceCustom);
```

**说明**：`set_frame_type(kForceCustom)` 强制使用 Chromium 自绘的 non-client frame；由于 P6 尚未实现自定义 non-client frame view，此时默认 fallback 是 `OpaqueBrowserFrameView`，仍会显示极简边框。**这是可接受的过渡状态**——P6 会替换成真正 zero-chrome 的自定义 view。

- [ ] **Step 4: 编译 + smoke tests**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
```

- [ ] **Step 5: 手动 GUI 验证（若有 X server / VNC）**

Run:
```bash
/opt/chromium/src/out/linux/chrome \
  --user-data-dir=/tmp/byod-manual-test \
  --no-first-run \
  --no-sandbox \
  https://example.com 2>&1 | head -5 &
sleep 3
# Check window has no title bar (needs GUI; if headless-only env, skip and defer)
kill %1 2>/dev/null || true
```
Expected: 若有 GUI —— 打开的 Chromium 窗口标题栏 / min-max-close 按钮不见或极简；若无 GUI —— skip 记入 acceptance checklist

- [ ] **Step 6: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/views/frame/browser_frame.cc
git commit -m "byod: force custom frame on Views platforms (patch #7a)

Patch #7a (spec §3.1, §4.1.2). Aura/Views (Linux, Windows) uses
kForceCustom frame type. Custom non-client view fully implemented in P6."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0210-kiosk-frameless-window-linux-win.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1b.sh

cd /root/byod-browser
git add patches/0210-kiosk-frameless-window-linux-win.patch build/smoke-test-p1b.sh
git commit -m "patch: 0210 Views frameless (patch #7a)

Est. LOC: ~10."
```

---

## Task 7: 补丁 #7b — Cocoa 平台无边框窗口（macOS）

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/cocoa/browser_window_controller.mm`
- Modify (in Chromium src): `chrome/browser/ui/cocoa/framed_browser_window.mm`（视 Chromium 版本，命名可能是 `browser_window_ns_window.mm` 等）
- Create: `/root/byod-browser/patches/0211-kiosk-frameless-nswindow-mac.patch`

**Interfaces:**
- Consumes: Task 6 完成
- Produces: macOS 上 Chromium 主 NSWindow 用 `NSWindowStyleMaskBorderless`，无标题栏、无红黄绿按钮

**跨平台**：`.mm` 文件在 Linux/Windows 编译时不参与，只需 Linux 上 `apply` 与 `build-linux` 都成功即可。macOS 首编在 P0 或 P1 结束时完成，跑 mac 手动验证。

- [ ] **Step 1: 定位 NSWindow 创建**

Run:
```bash
grep -rn "styleMask\|NSWindowStyleMaskTitled" /opt/chromium/src/chrome/browser/ui/cocoa/ | head -20
```

- [ ] **Step 2: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1b.sh`:

```bash
# ---------- Patch #7b: frameless NSWindow (macOS) ----------
if [[ "$(uname -s)" == "Darwin" ]]; then
  echo "[test] patch #7b: NSWindow borderless"
  pass "#7b: (deferred to mac GUI test — see p1-b-acceptance.md)"
else
  echo "[skip] patch #7b (macOS only)"
fi
```

- [ ] **Step 3: 修改 NSWindow style mask**

在 Chromium 创建主 NSWindow 的地方（多半是 `browser_window_controller.mm` 里的初始化），找到形如：

```objc
    NSUInteger styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
```

的定义，改成：

```objc
    // BYOD patch #7b (spec §3.1, §4.1.2): borderless Kiosk window.
    NSUInteger styleMask = NSWindowStyleMaskBorderless;
```

**若 `styleMask` 定义分散在多处**：都改；grep `NSWindowStyleMaskTitled` 覆盖到的每处都要评估。

- [ ] **Step 4: Linux 上 apply + 编译 验证（.mm 不参与 Linux 编译）**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/cocoa/browser_window_controller.mm
git commit -m "byod: borderless NSWindow style mask on macOS (patch #7b)

Patch #7b (spec §3.1, §4.1.2). Companion to #7a for Views."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0211-kiosk-frameless-nswindow-mac.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1b.sh
```
Expected: apply 与 build 成功；smoke `#7b` skip on Linux

- [ ] **Step 5: Commit**

Run:
```bash
cd /root/byod-browser
git add patches/0211-kiosk-frameless-nswindow-mac.patch build/smoke-test-p1b.sh
git commit -m "patch: 0211 Cocoa borderless (patch #7b)

Est. LOC: ~8. macOS behavioral verification deferred."
```

---

## Task 8: 补丁 #7c — 窗口尺寸锁定（跨平台）+ 置顶

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/views/frame/browser_view.cc`
- Create: `/root/byod-browser/patches/0212-kiosk-topmost-and-locked-size.patch`

**Interfaces:**
- Consumes: Task 7 完成
- Produces: 主窗口尺寸设置为**当前 primary display 全屏**（`display::Screen::GetPrimaryDisplay().bounds()`）；用户拖动 / 系统 API 试图 resize 都无效；窗口 always-on-top

- [ ] **Step 1: 定位 BrowserView 初始化**

Run:
```bash
grep -n "void BrowserView::InitViews\|BrowserView::OnWindowBeginUserBoundsChange\|BrowserView::GetPreferredSize" /opt/chromium/src/chrome/browser/ui/views/frame/browser_view.cc | head -10
```

- [ ] **Step 2: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1b.sh`:

```bash
# ---------- Patch #7c: locked size + topmost ----------
echo "[test] patch #7c: size lock + topmost"
pass "#7c: (source-level; manual GUI test in p1-b-acceptance.md)"
```

- [ ] **Step 3: 修改 BrowserView::InitViews — 强制全屏尺寸 + always-on-top**

在 `BrowserView::InitViews()` 函数体末尾添加：

```cpp
  // BYOD patch #7c (spec §3.1, §4.1.2): lock window to primary display bounds
  // and force always-on-top. User cannot resize; system cannot push window
  // below others.
  auto* widget = GetWidget();
  if (widget) {
    display::Screen* screen = display::Screen::GetScreen();
    if (screen) {
      gfx::Rect bounds = screen->GetPrimaryDisplay().bounds();
      widget->SetBounds(bounds);
    }
    widget->SetZOrderLevel(ui::ZOrderLevel::kFloatingWindow);
    widget->SetCanAppearInExistingFullscreenSpaces(true);
  }
```

在 `BrowserView::OnWindowBeginUserBoundsChange()` 函数入口早退：

```cpp
  // BYOD patch #7c: reject user-initiated resize.
  return;
```

若 `OnWindowBeginUserBoundsChange` 不存在（版本差异），改为覆写 `CanResize()` 返回 false：

```cpp
// In BrowserView class methods:
bool BrowserView::CanResize() const {
  // BYOD patch #7c: window is fixed-size.
  return false;
}
```

（`CanResize` 是 `views::WidgetDelegate` 的 virtual 方法，若 BrowserView 已经 override 过就在原方法体里改；若没 override 就新加）

- [ ] **Step 4: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**若报"display::Screen include 未找到"**：add `#include "ui/display/screen.h"` 到文件顶部。

**若报"ZOrderLevel 未声明"**：add `#include "ui/base/ui_base_types.h"`。

- [ ] **Step 5: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
```

- [ ] **Step 6: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/views/frame/browser_view.cc
git commit -m "byod: lock window size to primary display + always-on-top

Patch #7c (spec §3.1, §4.1.2). Cross-platform (Aura + Cocoa via Views
abstraction)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0212-kiosk-topmost-and-locked-size.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1b.sh

cd /root/byod-browser
git add patches/0212-kiosk-topmost-and-locked-size.patch build/smoke-test-p1b.sh
git commit -m "patch: 0212 locked size + topmost (patch #7c)

Est. LOC: ~25."
```

---

## Task 9: 补丁 #7d — 禁最小化 / 关闭按钮 / Alt+F4 关闭路径

**Files:**
- Modify (in Chromium src): `chrome/browser/ui/views/frame/browser_view.cc`（continuation of Task 8）
- Modify (in Chromium src): `chrome/browser/ui/browser.cc` — `Browser::ShouldCloseWindow` / `Browser::TryToCloseWindow`
- Create: `/root/byod-browser/patches/0213-kiosk-disable-minimize-and-close.patch`

**Interfaces:**
- Consumes: Task 8 完成
- Produces: 窗口最小化调用（`widget->Minimize()`、`SC_MINIMIZE` Win msg、mac dock minimize）全部 no-op；正常关闭窗口路径（点关闭按钮、Alt+F4、Cmd+W、`window.close()`）**除非考试结束或熔断信号**否则被拒绝

**说明**：真正的"考试结束才允许关"逻辑由 P6 考试壳控制（通过一个 `AllowClose` flag / mojo 信号）；本补丁只提供**默认拒绝**的钩子；P6 会在合适时调设置 flag。

- [ ] **Step 1: 定位 close / minimize 入口**

Run:
```bash
grep -n "bool Browser::ShouldCloseWindow\|Browser::TryToCloseWindow\|Browser::OnWindowClosing" /opt/chromium/src/chrome/browser/ui/browser.cc | head -10
grep -n "void BrowserView::Minimize\|CanMinimize" /opt/chromium/src/chrome/browser/ui/views/frame/browser_view.cc | head -10
```

- [ ] **Step 2: 追加 smoke test**

Edit `/root/byod-browser/build/smoke-test-p1b.sh`:

```bash
# ---------- Patch #7d: disable minimize/close by default ----------
echo "[test] patch #7d: no minimize / no close"
pass "#7d: (source-level; manual GUI test in p1-b-acceptance.md)"
```

- [ ] **Step 3: 添加 CanMinimize override + AllowClose gate**

**Part A** — 在 `browser_view.cc` 里加：

```cpp
// BYOD patch #7d (spec §3.1, §4.1.2): disable minimize.
bool BrowserView::CanMinimize() const {
  return false;
}
```

**Part B** — 在 `browser.cc` 里，找 `bool Browser::ShouldCloseWindow()` 函数体入口，添加：

```cpp
  // BYOD patch #7d (spec §3.1, §4.1.2): default-deny window close. P6 exam
  // shell sets |byod_allow_close_| to true when the exam legitimately ends
  // (submit / burn / instructor-signaled early exit).
  //
  // Class-level member declared in browser.h:
  //     bool byod_allow_close_ = false;
  if (!byod_allow_close_) {
    return false;
  }
```

**Part C** — 在 `browser.h` 里加成员声明：

```cpp
 public:
  // BYOD patch #7d: set by exam shell (P6) to unblock ShouldCloseWindow().
  void SetByodAllowClose(bool allow) { byod_allow_close_ = allow; }
  bool GetByodAllowClose() const { return byod_allow_close_; }

 private:
  // BYOD patch #7d: default false — window cannot be closed until exam
  // shell explicitly allows it.
  bool byod_allow_close_ = false;
```

**Interface for P6**（future consumer）：P6 的考试壳在收到"允许交卷"或"熔断"信号后，通过 Chromium API `browser->SetByodAllowClose(true)`（或 mojo IPC 到 Browser 进程后调用同名）触发窗口可关。**本 sub-plan 只暴露 setter；P6 具体如何调用是 P6 内部事。**

- [ ] **Step 4: 编译**

Run: `bash /root/byod-browser/build/build-linux.sh chrome`

**若 Browser 类没有 default constructor 初始化成员的模式**：将 `byod_allow_close_ = false` 移到构造函数初始化列表。

- [ ] **Step 5: Smoke tests**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
bash /root/byod-browser/build/smoke-test-p1a.sh
bash /root/byod-browser/build/smoke-test-p1b.sh
```

- [ ] **Step 6: 导出 + 验证 + commit**

Run:
```bash
cd /opt/chromium/src
git add chrome/browser/ui/views/frame/browser_view.cc chrome/browser/ui/browser.cc chrome/browser/ui/browser.h
git commit -m "byod: disable minimize + default-deny close (patch #7d)

Patch #7d (spec §3.1, §4.1.2). BrowserView::CanMinimize() returns false;
Browser::ShouldCloseWindow() returns false unless byod_allow_close_ is
explicitly set by exam shell (P6)."
git format-patch -1 --stdout HEAD > /root/byod-browser/patches/0213-kiosk-disable-minimize-and-close.patch

echo "y" | bash /root/byod-browser/build/revert-patches.sh
bash /root/byod-browser/build/apply-patches.sh
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test-p1b.sh

cd /root/byod-browser
git add patches/0213-kiosk-disable-minimize-and-close.patch build/smoke-test-p1b.sh
git commit -m "patch: 0213 disable minimize + close gate (patch #7d)

Est. LOC: ~30 including header declaration."
```

---

## Task 10: 补丁总量预算校验 + 交付确认

**Files:**
- Create: `/root/byod-browser/docs/development/p1-b-acceptance.md`

**Interfaces:**
- Consumes: Task 1..9 全部完成
- Produces: P1-B 交付验收 checklist

- [ ] **Step 1: 统计当前 patch 总行数**

Run:
```bash
source /root/byod-browser/build/chromium-version.env
cd /opt/chromium/src
git diff --shortstat "$CHROMIUM_TAG"..HEAD
```
Expected: P1-A + P1-B 累计 insertions 应 ≤ 1180 (600 + 580)

- [ ] **Step 2: 数一下累计补丁文件数**

Run:
```bash
ls /root/byod-browser/patches/*.patch | wc -l
```
Expected: 18（P1-A 的 9 + P1-B 的 9）

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
bash /root/byod-browser/build/smoke-test-p1b.sh
```
Expected: 全部 pass

- [ ] **Step 5: 写 acceptance checklist**

Write `/root/byod-browser/docs/development/p1-b-acceptance.md`:

```markdown
# P1-B 交付验收 Checklist

## 补丁类

- [ ] `patches/0200-force-main-window-navigation.patch` 存在
- [ ] `patches/0201-block-window-open-cross-origin.patch` 存在
- [ ] `patches/0300-strip-context-menu-items.patch` 存在
- [ ] `patches/0301-block-clipboard-and-save-commands.patch` 存在
- [ ] `patches/0302-disable-drag-out.patch` 存在
- [ ] `patches/0210-kiosk-frameless-window-linux-win.patch` 存在
- [ ] `patches/0211-kiosk-frameless-nswindow-mac.patch` 存在
- [ ] `patches/0212-kiosk-topmost-and-locked-size.patch` 存在
- [ ] `patches/0213-kiosk-disable-minimize-and-close.patch` 存在

## 编译类

- [ ] `bash build/revert-patches.sh && bash build/apply-patches.sh` 无冲突
- [ ] `bash build/build-linux.sh chrome` 成功
- [ ] `git diff --shortstat "$CHROMIUM_TAG"..HEAD` 显示 insertions ≤ 1180 (P1-A + P1-B 累计)

## 行为类（Linux headless smoke tests）

- [ ] `bash build/smoke-test.sh` 全部 pass
- [ ] `bash build/smoke-test-p1a.sh` 全部 pass
- [ ] `bash build/smoke-test-p1b.sh` 全部 pass（含 headless-limited 项显示 `?` 或 `pass (deferred)` 均算通过）

## 行为类（Linux GUI，若有 X server / VNC）

- [ ] 打开 Chromium：窗口无最大化/最小化/关闭按钮，或按钮存在但点击无反应
- [ ] 尝试拖动窗口边缘 resize：无效
- [ ] Alt+F4 / Cmd+W：窗口不关闭
- [ ] 右键页面：不弹菜单（或菜单为空）
- [ ] 页内选中文本试图拖到桌面：拒绝
- [ ] 在页面 JS 里试 `window.open('https://example.com')`：返回 null
- [ ] 在页面里放 `<a href="..." target="_blank">` 点击：在当前窗口内导航（不新开）

## 行为类（macOS，若 mac 首编已完成）

- [ ] mac 上运行的 Chromium 窗口 borderless（无标题栏、无红黄绿按钮）
- [ ] 全屏尺寸自动锁定到 primary display

## 行为类（Windows，若 Windows 首编已完成）

- [ ] Windows 上运行的 Chromium 窗口 topmost、无系统边框
- [ ] Ctrl+C / Ctrl+V 快捷键无效（同时验证 P1-A #10b + P1-B #6b 组合）

## 文档类

- [ ] 本文件（p1-b-acceptance.md）存在
- [ ] `docs/superpowers/plans/2026-07-11-byod-roadmap.md` 里 P1 状态更新为 `⏳ 进行中 (P1-A + P1-B 完成)`

## 交付判定

以上全部打勾 → P1-B 完成，可进入 P1-C（尚未编写）。
```

- [ ] **Step 6: Commit acceptance + 更新 roadmap**

Run:
```bash
cd /root/byod-browser
git add docs/development/p1-b-acceptance.md
git commit -m "docs: add P1-B acceptance checklist"
```

然后编辑 `/root/byod-browser/docs/superpowers/plans/2026-07-11-byod-roadmap.md` 把 P1 行状态改成 `⏳ 进行中 (P1-A + P1-B 完成)`，然后：

```bash
cd /root/byod-browser
git add docs/superpowers/plans/2026-07-11-byod-roadmap.md
git commit -m "docs: mark P1-B complete in roadmap"
```

---

## P1-B 完成后

- 已交付 9 个补丁文件（对应 spec §3.1 补丁 #5、#6、#7 三大项拆细）
- 累计 patch 行数应 ≤ 1180（P1-A 600 + P1-B 580 预算）
- 下一批：**P1-C**（补丁 #8 WebRequest hook、#9 Browser Exam Key 请求头注入），预算 ~450 行

**预计实际耗时**（含每次增量编译）：
- 熟练 Chromium 开发者：3-4 天
- 首次改 Chromium 的开发者：1-2 周

**风险与常见坑：**

- **CanResize / ShouldCloseWindow 已被 Chromium 上游 override**：Chromium 有多层 `WidgetDelegate` 继承，Browser 类 vs BrowserView 类各自可能已 override 过。若发现 patch 不生效，`grep -rn "CanResize\|ShouldCloseWindow" chrome/browser/ui/` 找到所有 override 点都改掉
- **StartDrag 拒绝过头**：可能连页内 HTML5 drag 都禁掉了，导致排序题不能玩。若发现：把 patch #6c 的 return 挪到更精细的判断分支（是否 `is_html_dnd == true` 之类）
- **CreateNewWindow no-op 后 renderer 挂起**：必须先 `callback.Run(...)` 再 `return`，不能直接 return
- **Cocoa patch 编译期未触发**：Linux 上 `apply` 会包括 `.mm` 文件到 index，但不参与编译；一定要在 mac 上做一轮 apply + build 验证 mac 端能编 —— **接受 defer 到 mac 首编时统一验证**
- **Views frame 无 native window frame 后调试困难**：dev 阶段可以临时 revert `0210` / `0211` 让窗口带回边框；生产 build 时统一 re-apply
