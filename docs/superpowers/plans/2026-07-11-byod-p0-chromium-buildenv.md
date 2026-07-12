# P0 · Chromium 构建环境 & 补丁管理 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立能可重复构建 fork Chromium 的开发环境，跑通 `git format-patch` 补丁管理工作流，为 P1 及后续 sub-plan 提供基础设施。

**Architecture:** 使用 Chromium 官方 `depot_tools` + `gn`/`ninja` 构建链；补丁以 `git format-patch` 系列文件形式存放在 `patches/` 目录，由脚本 `apply-patches.sh` 应用到 pinned Chromium tag；CI 缓存 `src/out` 与 depot_tools 减少重编时间。

**Tech Stack:** depot_tools、gn、ninja、Python 3、Bash、git、（可选）ccache/sccache

## Global Constraints

- Chromium 补丁总量 ≤ 3000 行（净增，不含 rebase 调整）
- Chromium rebase 节奏 = stable channel 12 周对齐
- Chromium 补丁只做"删/禁"和"钩子暴露"，不写业务
- 所有二进制无签名（v1），架构预留签名钩子
- 开发目录：`/root/byod-browser/`；Chromium 源码另置于 `/opt/chromium/src/`（Chromium 需要 40GB+ 空间，不放在 repo 内）
- Chromium 版本：**pin 到具体 stable tag**（例如 `130.0.6723.116`），不用 tip-of-tree
- 构建目标：**Windows x64 (release)** 与 **macOS arm64 + x64 (release)**

---

## File Structure

**本 sub-plan 创建/管理的文件：**

```
/root/byod-browser/
├── patches/                          # Chromium 补丁存放目录（P1 才开始有内容）
│   └── .gitkeep
├── build/
│   ├── chromium-version.env          # pin 的 Chromium tag
│   ├── apply-patches.sh              # 把 patches/*.patch 应用到 src/
│   ├── revert-patches.sh             # 撤销所有 patch，回到 vanilla
│   ├── build-linux.sh                # Linux dev build（快速迭代用）
│   ├── build-windows.sh              # Windows cross-build 或本地 build 说明
│   ├── build-macos.sh                # macOS build
│   ├── gn-args-linux.gn              # gn args for Linux
│   ├── gn-args-windows.gn            # gn args for Windows
│   ├── gn-args-macos.gn              # gn args for macOS
│   └── README.md                     # 环境搭建 + 常见问题
├── ci/
│   ├── github-actions/
│   │   └── build.yml                 # CI workflow
│   └── cache-keys.sh                 # 生成 cache key 的脚本
└── docs/
    └── development/
        ├── build-environment.md      # 详细的开发者环境搭建指南
        ├── patch-workflow.md         # patch 增删改流程
        └── rebase-guide.md           # 12 周 rebase 手册
```

**Chromium 源码目录（不在 repo 内）：**
- Linux/macOS：`/opt/chromium/`
- Windows：`C:\chromium\`（约定，见 build 脚本）

**磁盘空间要求：**
- Chromium checkout：~30 GB
- Build output（每目标平台）：~15-30 GB
- ccache：~20 GB
- 建议至少 **200 GB 空闲**

**内存与 CPU：**
- 首编：16 GB RAM 勉强、32 GB 舒服；16 核 CPU 编译 2-4 小时
- 增量编译：几秒到几分钟

---

## Task 1: 初始化仓库结构

**Files:**
- Create: `/root/byod-browser/.gitignore`
- Create: `/root/byod-browser/README.md`
- Create: `/root/byod-browser/patches/.gitkeep`
- Create: `/root/byod-browser/build/.gitkeep`
- Create: `/root/byod-browser/ci/.gitkeep`
- Create: `/root/byod-browser/docs/development/.gitkeep`

**Interfaces:**
- Consumes: none
- Produces: 目录骨架

- [ ] **Step 1: 检查当前目录状态**

Run: `ls -la /root/byod-browser/`
Expected: 只有 `docs/` 目录（含 spec 与 plans）

- [ ] **Step 2: 初始化 git 仓库（若尚未初始化）**

Run:
```bash
cd /root/byod-browser
git init 2>&1 | head -3
git config user.name "Byod Dev" 2>/dev/null || true
git config user.email "dev@byod.local" 2>/dev/null || true
```
Expected: `Initialized empty Git repository` 或已存在的提示

- [ ] **Step 3: 创建 .gitignore**

Write `/root/byod-browser/.gitignore` with content:

```
# Build outputs
/build/out/
/build/tmp/
*.o
*.obj
*.pyc
__pycache__/

# IDE
.vscode/
.idea/
*.swp
*.swo
.DS_Store

# OS
Thumbs.db

# Local overrides
*.local
.env.local
```

- [ ] **Step 4: 创建顶层 README.md**

Write `/root/byod-browser/README.md`:

```markdown
# BYOD Exam Browser

基于 Fork Chromium 的 BYOD 考试浏览器（Windows + macOS）。

## 目录

- [`docs/superpowers/specs/`](docs/superpowers/specs/) — 架构设计与 spec
- [`docs/superpowers/plans/`](docs/superpowers/plans/) — 实施计划（roadmap + P0..P11）
- [`docs/development/`](docs/development/) — 开发者环境搭建、patch 流程、rebase 手册
- [`patches/`](patches/) — Chromium 补丁集（P1 交付）
- [`build/`](build/) — 构建脚本与 gn args
- [`ci/`](ci/) — CI 配置

## 快速开始

见 [`docs/development/build-environment.md`](docs/development/build-environment.md)。

## 项目状态

v1 开发中，参见 [`docs/superpowers/plans/2026-07-11-byod-roadmap.md`](docs/superpowers/plans/2026-07-11-byod-roadmap.md)。
```

- [ ] **Step 5: 创建占位目录**

Run:
```bash
mkdir -p /root/byod-browser/patches /root/byod-browser/build /root/byod-browser/ci /root/byod-browser/docs/development
touch /root/byod-browser/patches/.gitkeep /root/byod-browser/build/.gitkeep /root/byod-browser/ci/.gitkeep /root/byod-browser/docs/development/.gitkeep
```
Expected: 无输出

- [ ] **Step 6: 首次 commit**

Run:
```bash
cd /root/byod-browser
git add -A
git commit -m "chore: initialize repo skeleton for P0"
```
Expected: `create mode` 若干；commit hash 输出

---

## Task 2: 确定 Chromium 目标版本并记录

**Files:**
- Create: `/root/byod-browser/build/chromium-version.env`

**Interfaces:**
- Consumes: none
- Produces: `CHROMIUM_TAG`, `CHROMIUM_MILESTONE`（供后续脚本 source）

- [ ] **Step 1: 编写测试脚本先验证格式**

Write `/tmp/test-chromium-version.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
source /root/byod-browser/build/chromium-version.env

# 校验 CHROMIUM_TAG 是形如 130.0.6723.116 的四段版本
if ! [[ "$CHROMIUM_TAG" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "FAIL: CHROMIUM_TAG format invalid: $CHROMIUM_TAG"
  exit 1
fi

# 校验 CHROMIUM_MILESTONE 是纯数字且 = TAG 首段
tag_major="${CHROMIUM_TAG%%.*}"
if [ "$CHROMIUM_MILESTONE" != "$tag_major" ]; then
  echo "FAIL: CHROMIUM_MILESTONE ($CHROMIUM_MILESTONE) != TAG major ($tag_major)"
  exit 1
fi

echo "PASS: version file valid"
```

- [ ] **Step 2: 运行测试验证失败**

Run: `bash /tmp/test-chromium-version.sh`
Expected: FAIL, `chromium-version.env: No such file or directory`

- [ ] **Step 3: 查询当前 Chromium stable 版本**

Run:
```bash
curl -s https://chromiumdash.appspot.com/fetch_releases?channel=Stable\&platform=Linux\&num=1 | head -c 500
```
Expected: JSON 输出含 `"version": "..."` 字段。**记录当前 stable 版本号（例如 `130.0.6723.116`）**。若网络不通，退回使用一个已知稳定版本 `130.0.6723.116`。

- [ ] **Step 4: 写 chromium-version.env**

Write `/root/byod-browser/build/chromium-version.env`（**把 CHROMIUM_TAG 换成 Step 3 查到的实际版本**）:

```bash
# Chromium stable tag we pin against.
# Update this file when performing a 12-week rebase.
# See docs/development/rebase-guide.md for the process.
export CHROMIUM_TAG="130.0.6723.116"
export CHROMIUM_MILESTONE="130"

# The date this version was pinned (for tracking rebase cadence).
export CHROMIUM_PIN_DATE="2026-07-11"

# Next planned rebase (12 weeks after pin date).
export CHROMIUM_NEXT_REBASE="2026-10-03"
```

- [ ] **Step 5: 运行测试验证通过**

Run: `bash /tmp/test-chromium-version.sh`
Expected: `PASS: version file valid`

- [ ] **Step 6: Commit**

Run:
```bash
cd /root/byod-browser
git add build/chromium-version.env
git commit -m "build: pin Chromium version to 130.0.6723.116"
rm /tmp/test-chromium-version.sh
```

---

## Task 3: 编写 depot_tools 安装脚本

**Files:**
- Create: `/root/byod-browser/build/install-depot-tools.sh`

**Interfaces:**
- Consumes: `CHROMIUM_TAG` (from Task 2)
- Produces: `depot_tools` 可执行环境，`PATH` 指引

- [ ] **Step 1: 写脚本**

Write `/root/byod-browser/build/install-depot-tools.sh`:

```bash
#!/usr/bin/env bash
# Install Chromium depot_tools into $DEPOT_TOOLS_DIR (default /opt/depot_tools).
# Idempotent: safe to re-run.
set -euo pipefail

DEPOT_TOOLS_DIR="${DEPOT_TOOLS_DIR:-/opt/depot_tools}"
DEPOT_TOOLS_URL="https://chromium.googlesource.com/chromium/tools/depot_tools.git"

if [ -d "$DEPOT_TOOLS_DIR/.git" ]; then
  echo "[info] depot_tools already at $DEPOT_TOOLS_DIR, updating..."
  git -C "$DEPOT_TOOLS_DIR" fetch --depth=1 origin main
  git -C "$DEPOT_TOOLS_DIR" reset --hard origin/main
else
  echo "[info] cloning depot_tools into $DEPOT_TOOLS_DIR"
  parent_dir="$(dirname "$DEPOT_TOOLS_DIR")"
  mkdir -p "$parent_dir"
  git clone --depth=1 "$DEPOT_TOOLS_URL" "$DEPOT_TOOLS_DIR"
fi

# Sanity check
if [ ! -x "$DEPOT_TOOLS_DIR/gclient" ]; then
  echo "[error] gclient not found in $DEPOT_TOOLS_DIR after install"
  exit 1
fi

cat <<EOF

[success] depot_tools installed at $DEPOT_TOOLS_DIR

Add to your shell profile:
    export PATH="\$PATH:$DEPOT_TOOLS_DIR"
    export DEPOT_TOOLS_UPDATE=1

Then run: gclient --version
EOF
```

- [ ] **Step 2: 使脚本可执行**

Run:
```bash
chmod +x /root/byod-browser/build/install-depot-tools.sh
```

- [ ] **Step 3: 执行脚本安装 depot_tools**

Run:
```bash
bash /root/byod-browser/build/install-depot-tools.sh
```
Expected: `[success] depot_tools installed at /opt/depot_tools`

- [ ] **Step 4: 验证 depot_tools 可用**

Run:
```bash
export PATH="$PATH:/opt/depot_tools"
gclient --version
```
Expected: 一行版本号（如 `gclient.py 0.7`）；若失败请回到 Step 3 检查

- [ ] **Step 5: Commit**

Run:
```bash
cd /root/byod-browser
git add build/install-depot-tools.sh
git commit -m "build: add depot_tools installer script"
```

---

## Task 4: 编写 Chromium fetch 脚本

**Files:**
- Create: `/root/byod-browser/build/fetch-chromium.sh`

**Interfaces:**
- Consumes: `CHROMIUM_TAG` (Task 2)、`depot_tools` (Task 3)
- Produces: `/opt/chromium/src/` 上锁到指定 tag 的 vanilla Chromium 源码树

- [ ] **Step 1: 写脚本**

Write `/root/byod-browser/build/fetch-chromium.sh`:

```bash
#!/usr/bin/env bash
# Fetch Chromium source tree pinned to $CHROMIUM_TAG.
# Places source at $CHROMIUM_SRC_ROOT (default /opt/chromium).
# Idempotent-ish: if already fetched, ensures the correct tag is checked out.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/chromium-version.env"

CHROMIUM_SRC_ROOT="${CHROMIUM_SRC_ROOT:-/opt/chromium}"
DEPOT_TOOLS_DIR="${DEPOT_TOOLS_DIR:-/opt/depot_tools}"

if [ ! -d "$DEPOT_TOOLS_DIR" ]; then
  echo "[error] depot_tools not found at $DEPOT_TOOLS_DIR. Run install-depot-tools.sh first."
  exit 1
fi

export PATH="$DEPOT_TOOLS_DIR:$PATH"
export DEPOT_TOOLS_UPDATE=1

mkdir -p "$CHROMIUM_SRC_ROOT"
cd "$CHROMIUM_SRC_ROOT"

if [ ! -d "$CHROMIUM_SRC_ROOT/src" ]; then
  echo "[info] initial fetch of Chromium (this takes 30-90 minutes and ~30GB)..."
  # --no-history keeps checkout smaller; drop it if you need git log
  fetch --no-history --nohooks chromium
else
  echo "[info] chromium source tree present, syncing..."
fi

cd "$CHROMIUM_SRC_ROOT/src"

# Checkout pinned tag
echo "[info] checking out tag $CHROMIUM_TAG..."
git fetch --depth=1 origin "refs/tags/$CHROMIUM_TAG:refs/tags/$CHROMIUM_TAG"
git checkout -f "$CHROMIUM_TAG"

# Sync deps (runs hooks)
echo "[info] running gclient sync..."
gclient sync --with_branch_heads --with_tags --nohooks

# Run hooks (downloads binaries like clang, sysroots, etc.)
echo "[info] running gclient runhooks..."
gclient runhooks

echo "[success] Chromium $CHROMIUM_TAG ready at $CHROMIUM_SRC_ROOT/src"
```

- [ ] **Step 2: 使脚本可执行**

Run:
```bash
chmod +x /root/byod-browser/build/fetch-chromium.sh
```

- [ ] **Step 3: Dry-run 检查脚本语法**

Run:
```bash
bash -n /root/byod-browser/build/fetch-chromium.sh && echo "syntax OK"
```
Expected: `syntax OK`

- [ ] **Step 4: 实际执行 fetch（长时间任务！）**

⚠️ **注意：这一步会下载 30GB 以上数据，耗时 30-90 分钟。建议前台或独立 shell 执行，并确保磁盘剩余空间 > 100GB。**

Run:
```bash
bash /root/byod-browser/build/fetch-chromium.sh
```
Expected: 最终打印 `[success] Chromium 130.0.6723.116 ready at /opt/chromium/src`

若中途中断：直接重新运行脚本，`fetch` 会续传，`gclient sync` 会补齐。

- [ ] **Step 5: 验证 Chromium 源码可用**

Run:
```bash
ls /opt/chromium/src/chrome/browser | head -5
git -C /opt/chromium/src describe --tags
```
Expected:
```
BUILD.gn
about_flags.cc
about_flags.h
accessibility/
active_use_util.cc
```
和形如 `130.0.6723.116` 的 tag 名

- [ ] **Step 6: Commit**

Run:
```bash
cd /root/byod-browser
git add build/fetch-chromium.sh
git commit -m "build: add Chromium fetch script"
```

---

## Task 5: 编写 gn args 模板（Linux 快速迭代用）

**Files:**
- Create: `/root/byod-browser/build/gn-args-linux.gn`

**Interfaces:**
- Consumes: none
- Produces: Linux release build 的 gn 参数集

**说明：** Linux build 主要用于快速验证补丁改动（比 Win/mac 编译快得多）；生产目标是 Win + macOS，但 Linux 上跑通再交叉编译或到目标平台上编译。

- [ ] **Step 1: 写 gn args**

Write `/root/byod-browser/build/gn-args-linux.gn`:

```gn
# gn args for Linux dev builds of BYOD Exam Browser.
# Used by build-linux.sh with `gn gen out/linux --args="$(cat gn-args-linux.gn)"`.
#
# Optimized for fast incremental builds (development iteration).
# For release testing, override with --args="is_official_build=true"

# Base
target_os = "linux"
target_cpu = "x64"

# Release-like but without official bloat
is_debug = false
is_component_build = true
symbol_level = 1

# Skip everything we don't need for exam browser
enable_nacl = false
enable_widevine = false
proprietary_codecs = false
ffmpeg_branding = "Chromium"

# Fast linker
use_lld = true

# Skip tests we don't run
enable_hangout_services_extension = false

# Reduce binary size a bit
blink_symbol_level = 0
```

- [ ] **Step 2: 创建 gn args for Windows**

Write `/root/byod-browser/build/gn-args-windows.gn`:

```gn
# gn args for Windows production builds.

target_os = "win"
target_cpu = "x64"

is_official_build = true
is_debug = false
symbol_level = 1

# Chromium official build strips a lot; we mostly follow it
enable_nacl = false
enable_widevine = false
proprietary_codecs = true
ffmpeg_branding = "Chrome"

# Enforce CIG (Code Integrity Guard) for exam-critical process
# Applied via build flag; see chrome/browser/BUILD.gn changes in P1

# Chromium requires MSVC (Visual Studio Build Tools 2022 + Win10 SDK 10.0.22621.0)
```

- [ ] **Step 3: 创建 gn args for macOS**

Write `/root/byod-browser/build/gn-args-macos.gn`:

```gn
# gn args for macOS production builds.

target_os = "mac"
target_cpu = "arm64"  # override with target_cpu="x64" for Intel Mac

is_official_build = true
is_debug = false
symbol_level = 1

enable_nacl = false
enable_widevine = false
proprietary_codecs = true
ffmpeg_branding = "Chrome"

# Hardened runtime + arm64 by default
# Notarization is done post-build in P10, not by gn
```

- [ ] **Step 4: Commit**

Run:
```bash
cd /root/byod-browser
git add build/gn-args-linux.gn build/gn-args-windows.gn build/gn-args-macos.gn
git commit -m "build: add gn args templates for linux/windows/macos"
```

---

## Task 6: 编写 build-linux.sh 并验证 vanilla 编译成功

**Files:**
- Create: `/root/byod-browser/build/build-linux.sh`

**Interfaces:**
- Consumes: `CHROMIUM_SRC_ROOT`、`gn-args-linux.gn`
- Produces: `$CHROMIUM_SRC_ROOT/src/out/linux/chrome` 可执行文件

- [ ] **Step 1: 写脚本**

Write `/root/byod-browser/build/build-linux.sh`:

```bash
#!/usr/bin/env bash
# Build Chromium on Linux (dev target).
# Usage: bash build-linux.sh [target]  (default target: chrome)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/chromium-version.env"

CHROMIUM_SRC_ROOT="${CHROMIUM_SRC_ROOT:-/opt/chromium}"
DEPOT_TOOLS_DIR="${DEPOT_TOOLS_DIR:-/opt/depot_tools}"
OUT_DIR="${OUT_DIR:-out/linux}"
TARGET="${1:-chrome}"

export PATH="$DEPOT_TOOLS_DIR:$PATH"

cd "$CHROMIUM_SRC_ROOT/src"

# Generate build config if missing
if [ ! -f "$OUT_DIR/args.gn" ]; then
  echo "[info] generating $OUT_DIR/args.gn from template..."
  mkdir -p "$OUT_DIR"
  cp "$SCRIPT_DIR/gn-args-linux.gn" "$OUT_DIR/args.gn"
  gn gen "$OUT_DIR"
fi

echo "[info] building target: $TARGET (this can take 2-4 hours on first run)"
autoninja -C "$OUT_DIR" "$TARGET"

echo "[success] built: $CHROMIUM_SRC_ROOT/src/$OUT_DIR/$TARGET"
```

- [ ] **Step 2: 使脚本可执行**

Run:
```bash
chmod +x /root/byod-browser/build/build-linux.sh
```

- [ ] **Step 3: 检查语法**

Run: `bash -n /root/byod-browser/build/build-linux.sh && echo OK`
Expected: `OK`

- [ ] **Step 4: 执行首次 build（长时间任务）**

⚠️ **注意：首编耗时 2-4 小时，占用 CPU 100% + 内存 8-32GB + 磁盘 15-30GB。执行前先确认硬件条件。**

Run:
```bash
bash /root/byod-browser/build/build-linux.sh chrome
```
Expected: 最终 `[success] built: /opt/chromium/src/out/linux/chrome`

- [ ] **Step 5: 验证 vanilla Chromium 能启动（headless）**

Run:
```bash
/opt/chromium/src/out/linux/chrome --headless --disable-gpu --dump-dom https://example.com 2>&1 | head -3
```
Expected: HTML dump 前几行（含 `<!DOCTYPE html>` 或类似）

- [ ] **Step 6: Commit**

Run:
```bash
cd /root/byod-browser
git add build/build-linux.sh
git commit -m "build: add Linux build script"
```

---

## Task 7: 编写 Windows build 脚本（占位 + 文档）

**Files:**
- Create: `/root/byod-browser/build/build-windows.ps1`

**Interfaces:**
- Consumes: `CHROMIUM_SRC_ROOT`、`gn-args-windows.gn`
- Produces: Windows build 入口

**说明：** Windows build 必须在 Windows 机器上执行（不能 Linux 交叉编译 Chromium），本 sub-plan 只提供脚本 + 环境搭建文档；实际首编在 P1 或以后一次专门任务里完成。

- [ ] **Step 1: 写 PowerShell 脚本**

Write `/root/byod-browser/build/build-windows.ps1`:

```powershell
# Build Chromium on Windows.
# Requires: Visual Studio 2022 Build Tools + Win10 SDK 10.0.22621.0 + depot_tools in PATH
# Usage: powershell -File build-windows.ps1 [-Target chrome]
[CmdletBinding()]
param(
    [string]$Target = "chrome"
)
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Load pinned version
$envFile = Join-Path $ScriptDir "chromium-version.env"
if (-not (Test-Path $envFile)) {
    throw "chromium-version.env not found at $envFile"
}
Get-Content $envFile | Where-Object { $_ -match '^\s*export\s+(\w+)="?([^"]*)"?' } | ForEach-Object {
    Set-Variable -Name $Matches[1] -Value $Matches[2] -Scope Script
}

$SrcRoot = if ($env:CHROMIUM_SRC_ROOT) { $env:CHROMIUM_SRC_ROOT } else { "C:\chromium" }
$OutDir = "out\windows"

Set-Location "$SrcRoot\src"

if (-not (Test-Path "$OutDir\args.gn")) {
    Write-Host "[info] generating $OutDir\args.gn from template..."
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    Copy-Item "$ScriptDir\gn-args-windows.gn" "$OutDir\args.gn"
    gn gen $OutDir
}

Write-Host "[info] building target: $Target (first build: 3-6 hours)"
autoninja -C $OutDir $Target

Write-Host "[success] built: $SrcRoot\src\$OutDir\$Target.exe"
```

- [ ] **Step 2: Commit**

Run:
```bash
cd /root/byod-browser
git add build/build-windows.ps1
git commit -m "build: add Windows build script (PowerShell)"
```

---

## Task 8: 编写 macOS build 脚本（占位 + 文档）

**Files:**
- Create: `/root/byod-browser/build/build-macos.sh`

**Interfaces:**
- Consumes: `CHROMIUM_SRC_ROOT`、`gn-args-macos.gn`
- Produces: macOS build 入口

**说明：** macOS build 必须在 macOS 机器上（Xcode 15+），需要 arm64 mac 才能编 arm64 build。

- [ ] **Step 1: 写脚本**

Write `/root/byod-browser/build/build-macos.sh`:

```bash
#!/usr/bin/env bash
# Build Chromium on macOS.
# Requires: macOS with Xcode 15+, depot_tools in PATH.
# Usage: bash build-macos.sh [target] [arch]
#   target defaults to "chrome"
#   arch defaults to "arm64"; use "x64" for Intel Mac
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/chromium-version.env"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[error] macOS build must run on macOS. Current OS: $(uname -s)"
  exit 1
fi

CHROMIUM_SRC_ROOT="${CHROMIUM_SRC_ROOT:-/opt/chromium}"
DEPOT_TOOLS_DIR="${DEPOT_TOOLS_DIR:-/opt/depot_tools}"
TARGET="${1:-chrome}"
ARCH="${2:-arm64}"
OUT_DIR="out/macos-$ARCH"

export PATH="$DEPOT_TOOLS_DIR:$PATH"

cd "$CHROMIUM_SRC_ROOT/src"

if [ ! -f "$OUT_DIR/args.gn" ]; then
  echo "[info] generating $OUT_DIR/args.gn from template..."
  mkdir -p "$OUT_DIR"
  # Substitute target_cpu based on ARCH argument
  sed "s/target_cpu = \"arm64\"/target_cpu = \"$ARCH\"/" "$SCRIPT_DIR/gn-args-macos.gn" > "$OUT_DIR/args.gn"
  gn gen "$OUT_DIR"
fi

echo "[info] building target: $TARGET (first build: 3-5 hours)"
autoninja -C "$OUT_DIR" "$TARGET"

echo "[success] built: $CHROMIUM_SRC_ROOT/src/$OUT_DIR/Chromium.app"
```

- [ ] **Step 2: 使脚本可执行**

Run:
```bash
chmod +x /root/byod-browser/build/build-macos.sh
```

- [ ] **Step 3: Commit**

Run:
```bash
cd /root/byod-browser
git add build/build-macos.sh
git commit -m "build: add macOS build script"
```

---

## Task 9: 编写 apply-patches.sh（补丁应用工作流）

**Files:**
- Create: `/root/byod-browser/build/apply-patches.sh`
- Create: `/root/byod-browser/build/revert-patches.sh`

**Interfaces:**
- Consumes: `patches/*.patch`（P1 交付；本 sub-plan 只用一个测试 patch 验证流程）
- Produces: 应用补丁的 Chromium 源码树

- [ ] **Step 1: 写 apply-patches.sh**

Write `/root/byod-browser/build/apply-patches.sh`:

```bash
#!/usr/bin/env bash
# Apply all patches from /root/byod-browser/patches/ to Chromium src tree in order.
# Uses `git am` for proper attribution and easy rebase.
# Idempotent-ish: aborts if patches already applied (via `git am --abort`-like check).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PATCHES_DIR="$REPO_ROOT/patches"

CHROMIUM_SRC_ROOT="${CHROMIUM_SRC_ROOT:-/opt/chromium}"
SRC_DIR="$CHROMIUM_SRC_ROOT/src"

if [ ! -d "$SRC_DIR" ]; then
  echo "[error] Chromium src not found at $SRC_DIR. Run fetch-chromium.sh first."
  exit 1
fi

# Check if we're on a clean state (at the pinned tag with no local commits ahead of it)
source "$SCRIPT_DIR/chromium-version.env"
cd "$SRC_DIR"

current_head="$(git rev-parse HEAD)"
pinned_head="$(git rev-parse "$CHROMIUM_TAG")"
if [ "$current_head" != "$pinned_head" ]; then
  # Check if HEAD is descendant of pinned tag (meaning patches already applied)
  if git merge-base --is-ancestor "$pinned_head" "$current_head"; then
    echo "[error] HEAD is ahead of $CHROMIUM_TAG — patches may already be applied."
    echo "        Run 'revert-patches.sh' first, then re-apply."
    exit 1
  else
    echo "[error] HEAD is not at $CHROMIUM_TAG. Run fetch-chromium.sh to reset."
    exit 1
  fi
fi

# Collect patches in sorted order (0001-foo.patch, 0002-bar.patch, ...)
shopt -s nullglob
patches=("$PATCHES_DIR"/*.patch)
shopt -u nullglob

if [ "${#patches[@]}" -eq 0 ]; then
  echo "[info] no patches to apply (patches dir empty)"
  exit 0
fi

echo "[info] applying ${#patches[@]} patch(es) to $SRC_DIR..."
for p in "${patches[@]}"; do
  echo "  → $(basename "$p")"
done

git am --3way "${patches[@]}"

echo "[success] all patches applied on top of $CHROMIUM_TAG"
```

- [ ] **Step 2: 写 revert-patches.sh**

Write `/root/byod-browser/build/revert-patches.sh`:

```bash
#!/usr/bin/env bash
# Reset Chromium src tree back to the pinned $CHROMIUM_TAG.
# Discards all locally-applied patches AND any uncommitted changes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/chromium-version.env"

CHROMIUM_SRC_ROOT="${CHROMIUM_SRC_ROOT:-/opt/chromium}"
SRC_DIR="$CHROMIUM_SRC_ROOT/src"

cd "$SRC_DIR"

echo "[warn] this will discard ALL local changes in $SRC_DIR"
read -r -p "Continue? [y/N] " ans
if [ "$ans" != "y" ] && [ "$ans" != "Y" ]; then
  echo "aborted"; exit 1
fi

# Abort any in-progress git am
if [ -d ".git/rebase-apply" ]; then
  git am --abort || true
fi

git reset --hard "$CHROMIUM_TAG"
git clean -fdx -e out/  # keep build outputs to avoid re-compile

echo "[success] reset to $CHROMIUM_TAG"
```

- [ ] **Step 3: 使脚本可执行**

Run:
```bash
chmod +x /root/byod-browser/build/apply-patches.sh /root/byod-browser/build/revert-patches.sh
```

- [ ] **Step 4: 空目录 dry-run**

Run:
```bash
bash /root/byod-browser/build/apply-patches.sh
```
Expected: `[info] no patches to apply (patches dir empty)`

- [ ] **Step 5: 创建测试 patch 验证流程**

Write `/root/byod-browser/patches/9999-test-noop.patch`（临时测试补丁，改一个不影响功能的注释文件）:

```
From: BYOD Dev <dev@byod.local>
Date: Fri, 11 Jul 2026 00:00:00 +0000
Subject: [PATCH] test: noop patch to verify apply-patches workflow

This patch is only for testing the P0 patch-management workflow.
It will be deleted before P1 begins.
---
 chrome/VERSION | 1 +
 1 file changed, 1 insertion(+)

diff --git a/chrome/VERSION b/chrome/VERSION
index 0000000..0000001 100644
--- a/chrome/VERSION
+++ b/chrome/VERSION
@@ -1,3 +1,4 @@
+# BYOD patch test marker
 MAJOR=130
 MINOR=0
 BUILD=6723
```

⚠️ **注意**：Step 5 的 patch 内容里的 `MAJOR=130` 等要匹配当前 pinned tag。如果 pin 的版本不同（Task 2 里选了别的），请对应调整。

- [ ] **Step 6: 应用测试 patch**

Run:
```bash
bash /root/byod-browser/build/apply-patches.sh
```
Expected: `[success] all patches applied on top of 130.0.6723.116`

若失败（比如 patch 中 hunk 与实际 VERSION 文件不匹配），说明 pinned tag 与 patch 期望的行不一致，需要手动重写这个测试 patch —— **这就是这一步的意义：暴露 pin/patch 匹配问题**。

- [ ] **Step 7: 验证 patch 已应用**

Run:
```bash
head -1 /opt/chromium/src/chrome/VERSION
git -C /opt/chromium/src log --oneline -1
```
Expected:
```
# BYOD patch test marker
<hash> test: noop patch to verify apply-patches workflow
```

- [ ] **Step 8: 验证 revert**

Run:
```bash
echo "y" | bash /root/byod-browser/build/revert-patches.sh
head -1 /opt/chromium/src/chrome/VERSION
```
Expected: `[success] reset to 130.0.6723.116`；`chrome/VERSION` 第一行恢复为 `MAJOR=130`

- [ ] **Step 9: 删除测试 patch**

Run:
```bash
rm /root/byod-browser/patches/9999-test-noop.patch
```

- [ ] **Step 10: Commit apply/revert scripts**

Run:
```bash
cd /root/byod-browser
git add build/apply-patches.sh build/revert-patches.sh
git commit -m "build: add patch apply/revert scripts with git-am workflow"
```

---

## Task 10: 编写增量构建 & 验证 patch 影响的 smoke test 脚本

**Files:**
- Create: `/root/byod-browser/build/smoke-test.sh`

**Interfaces:**
- Consumes: 已构建的 chrome 可执行文件
- Produces: 一个可复用的 smoke test（每次改 patch 后跑一遍）

- [ ] **Step 1: 写 smoke-test.sh**

Write `/root/byod-browser/build/smoke-test.sh`:

```bash
#!/usr/bin/env bash
# Smoke test: verify a built Chromium binary launches and can render a page.
# Usage: bash smoke-test.sh [path-to-chrome]
#        defaults to /opt/chromium/src/out/linux/chrome
set -euo pipefail

CHROME_BIN="${1:-/opt/chromium/src/out/linux/chrome}"

if [ ! -x "$CHROME_BIN" ]; then
  echo "[error] chrome binary not found or not executable: $CHROME_BIN"
  exit 1
fi

echo "[info] testing: $CHROME_BIN"

# Test 1: version
echo "  → version check..."
"$CHROME_BIN" --version | head -1

# Test 2: headless page render
echo "  → headless render test..."
tmp="$(mktemp)"
"$CHROME_BIN" --headless --disable-gpu --no-sandbox \
  --dump-dom https://example.com > "$tmp" 2>/dev/null

if ! grep -q "<h1>Example Domain</h1>" "$tmp"; then
  echo "[fail] headless render didn't return expected content"
  echo "output preview:"
  head -20 "$tmp"
  rm -f "$tmp"
  exit 1
fi
rm -f "$tmp"
echo "    OK"

echo "[success] smoke test passed"
```

- [ ] **Step 2: 使脚本可执行**

Run:
```bash
chmod +x /root/byod-browser/build/smoke-test.sh
```

- [ ] **Step 3: 运行 smoke test**

Run:
```bash
bash /root/byod-browser/build/smoke-test.sh
```
Expected:
```
[info] testing: /opt/chromium/src/out/linux/chrome
  → version check...
Chromium 130.0.6723.116
  → headless render test...
    OK
[success] smoke test passed
```

- [ ] **Step 4: Commit**

Run:
```bash
cd /root/byod-browser
git add build/smoke-test.sh
git commit -m "build: add smoke test script"
```

---

## Task 11: 写 build-environment.md（详细开发者指南）

**Files:**
- Create: `/root/byod-browser/docs/development/build-environment.md`

- [ ] **Step 1: 写文档**

Write `/root/byod-browser/docs/development/build-environment.md`:

```markdown
# 开发者环境搭建指南

本文档指引一个新开发者从零搭建 BYOD Exam Browser 的 Chromium fork 构建环境。

## 硬件要求

| 平台 | CPU | RAM | 磁盘 | 首编时长 |
|---|---|---|---|---|
| Linux (推荐迭代) | 8+ 核 | 16 GB (32 推荐) | 200 GB SSD | 2-4 小时 |
| Windows | 8+ 核 | 32 GB | 200 GB SSD | 3-6 小时 |
| macOS | Apple Silicon 或 Intel 8+ 核 | 16 GB (32 推荐) | 200 GB SSD | 3-5 小时 |

## 平台前置依赖

### Linux (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential curl git python3 python3-pip lsb-release \
                    file pkg-config ninja-build

# Chromium 官方 install-build-deps.sh（fetch 后运行）
# 见 fetch 步骤下方
```

### Windows

1. Visual Studio 2022 Build Tools（含 "Desktop development with C++"）
2. Windows 11 SDK 10.0.22621.0（其它 SDK 版本 gn 会报错）
3. Git for Windows
4. Python 3.10+
5. 设置系统环境变量：
   - `DEPOT_TOOLS_WIN_TOOLCHAIN=0`（用本地安装的 VS 而非 Google 内部工具链）
   - `vs2022_install=C:\Program Files\Microsoft Visual Studio\2022\BuildTools`

### macOS

1. Xcode 15+（App Store 安装完整版，命令行工具不够）
2. `xcode-select --install`
3. Homebrew（可选）

## 步骤

### 1. 克隆本项目

```bash
git clone <this-repo-url> /root/byod-browser
cd /root/byod-browser
```

### 2. 安装 depot_tools

```bash
bash build/install-depot-tools.sh
export PATH="$PATH:/opt/depot_tools"
export DEPOT_TOOLS_UPDATE=1
```

将上面两行 `export` 加入你的 shell profile（`.bashrc` / `.zshrc`）。

### 3. Fetch Chromium 源码

```bash
bash build/fetch-chromium.sh
```

这一步 30-90 分钟，下载 30GB+。

Linux 上额外一步（Chromium 需要一堆系统依赖）：

```bash
cd /opt/chromium/src
./build/install-build-deps.sh
```

### 4. 首编（vanilla，验证环境）

选一个平台跑一遍：

```bash
# Linux
bash build/build-linux.sh chrome

# Windows (PowerShell)
powershell -File build\build-windows.ps1 -Target chrome

# macOS
bash build/build-macos.sh chrome
```

首编 2-6 小时。跑完了执行 smoke test：

```bash
bash build/smoke-test.sh
```

### 5. 应用项目补丁（P1 后有内容）

```bash
bash build/apply-patches.sh
bash build/build-linux.sh chrome   # 增量编译，通常 10-30 分钟
bash build/smoke-test.sh
```

## 常见问题

### depot_tools 卡在 update

```bash
export DEPOT_TOOLS_UPDATE=0
```

### 磁盘空间不足

- 删除 `/opt/chromium/src/out/*` 的旧 build
- `git -C /opt/chromium/src gc --aggressive`

### 编译内存不足（OOM）

在 gn args 里加：`use_thin_lto = false`，减少并行 job：

```bash
autoninja -C out/linux -j 4 chrome
```

### 依赖 fetch 失败（GFW）

- 用官方推荐的 [Chromium mirror](https://chromium.googlesource.com/)
- 或参考 [中国镜像](https://mirrors.tuna.tsinghua.edu.cn/help/git-repo/)

### patch apply 冲突

如果 `apply-patches.sh` 报冲突：

```bash
cd /opt/chromium/src
git am --abort
```

然后阅读 [patch-workflow.md](patch-workflow.md)。

## 下一步

- 阅读 [patch-workflow.md](patch-workflow.md) 学习补丁增删改
- 阅读 [rebase-guide.md](rebase-guide.md) 学习 12 周 rebase 流程
- 开始 P1（Chromium Fork 补丁集）
```

- [ ] **Step 2: Commit**

Run:
```bash
cd /root/byod-browser
git add docs/development/build-environment.md
git commit -m "docs: add developer environment setup guide"
```

---

## Task 12: 写 patch-workflow.md（补丁增删改流程）

**Files:**
- Create: `/root/byod-browser/docs/development/patch-workflow.md`

- [ ] **Step 1: 写文档**

Write `/root/byod-browser/docs/development/patch-workflow.md`:

```markdown
# 补丁工作流

本文档说明如何在 Chromium 源码里做修改、导出为 patch 文件、加入 `patches/` 目录。

## 基本原则

- 补丁总量 ≤ 3000 行（净增，见 spec §3.1）
- 补丁只做"删/禁"和"钩子暴露"，**不写业务**
- 一个补丁 = 一个原子改动（对应 spec §3.1 表中的一项）
- 补丁文件按序号排列：`0001-<slug>.patch`, `0002-<slug>.patch`, ...
- 序号预留：`0001-0099` DevTools 相关，`0100-0199` scheme 相关，...（见下方分段）

## 序号分段（规范）

| 范围 | 类别 |
|---|---|
| 0001-0099 | 禁 DevTools 入口相关（patch #1, #2, #10） |
| 0100-0199 | scheme 白名单相关（patch #3, #4） |
| 0200-0299 | 窗口/导航策略（patch #5, #7） |
| 0300-0399 | 菜单/命令拦截（patch #6） |
| 0400-0499 | 网络层扩展（patch #8, #9） |
| 0500-0599 | 快捷键（patch #10, #11） |
| 0600-0699 | IPC 与 Guardian（patch #12） |
| 0700-0799 | Session / Config（patch #13） |
| 0800-0899 | 事件采集钩子（patch #14） |
| 0900-0999 | Attestation（patch #15） |

## 增加一个新补丁

```bash
# 1. 从干净状态开始
bash build/revert-patches.sh

# 2. 应用已有补丁
bash build/apply-patches.sh

# 3. 在 Chromium src 里做修改
cd /opt/chromium/src
vim chrome/browser/devtools/devtools_window.cc

# 4. 编译验证
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh

# 5. 提交为 git commit（暂时的）
cd /opt/chromium/src
git add -A
git commit -m "byod: disable OpenDevTools entrypoints (patch #1)"

# 6. 导出为 patch 文件
git format-patch -1 --stdout > /root/byod-browser/patches/0001-disable-devtools-entrypoints.patch

# 7. 验证 patch 可撤销并重新应用
cd /root/byod-browser
bash build/revert-patches.sh
bash build/apply-patches.sh   # 应该再次成功

# 8. Commit patch 到本仓库
git add patches/0001-disable-devtools-entrypoints.patch
git commit -m "patch: 0001 disable OpenDevTools entrypoints"
```

## 修改一个已有补丁

```bash
# 1. 从干净状态开始
bash build/revert-patches.sh
bash build/apply-patches.sh

# 2. 找到对应 patch 的 commit，用 interactive rebase 编辑
cd /opt/chromium/src
git log --oneline
# 找到那个 commit，例如 abcd123 就是 patch #1
git rebase -i abcd123^   # 把 abcd123 标记为 edit

# 3. 修改代码
vim chrome/browser/devtools/devtools_window.cc

# 4. amend commit
git add -A
git commit --amend --no-edit
git rebase --continue

# 5. 重新导出 patch
git format-patch \
  --start-number=1 -N \
  "$CHROMIUM_TAG"..HEAD \
  -o /root/byod-browser/patches/

# 6. Commit
cd /root/byod-browser
git add patches/
git commit -m "patch: update 0001 to also cover keyboard shortcuts"
```

## 删除一个补丁

直接删除 `patches/00XX-*.patch` 文件，git commit。

## 检查补丁总行数（3000 行预算）

```bash
cd /root/byod-browser
wc -l patches/*.patch | tail -1
# 但 wc 包含 patch metadata；更准确：
git diff --shortstat "$CHROMIUM_TAG"..HEAD | tail -1
```

## Debug 补丁编译失败

如果某个 patch 应用后编译失败：

1. `bash build/revert-patches.sh`
2. `bash build/apply-patches.sh 2>&1 | head -50` 看哪个 patch 出问题
3. 手动只应用出问题之前的 patches
4. 单独应用出问题的 patch 并调查

## 参考

- Chromium 源码检索：<https://source.chromium.org/chromium>
- Chromium Code Search：<https://chromium.googlesource.com/chromium/src/>
- `git format-patch` docs: <https://git-scm.com/docs/git-format-patch>
- `git am` docs: <https://git-scm.com/docs/git-am>
```

- [ ] **Step 2: Commit**

Run:
```bash
cd /root/byod-browser
git add docs/development/patch-workflow.md
git commit -m "docs: add patch workflow guide"
```

---

## Task 13: 写 rebase-guide.md（12 周 rebase 手册）

**Files:**
- Create: `/root/byod-browser/docs/development/rebase-guide.md`

- [ ] **Step 1: 写文档**

Write `/root/byod-browser/docs/development/rebase-guide.md`:

```markdown
# 12 周 Chromium Rebase 手册

每 12 周对齐一次 Chromium stable。本手册是完整流程 checklist。

## 前置

- 至少预留 2 个完整工作日
- 确保当前所有 patch 都能应用 + 编译 + smoke test 通过
- 备份 `patches/` 目录

## 步骤

### 1. 查最新 stable 版本

```bash
curl -s https://chromiumdash.appspot.com/fetch_releases?channel=Stable\&platform=Linux\&num=1
```

记录新版本，例如 `133.0.6900.100`。

### 2. 更新 chromium-version.env

```bash
cd /root/byod-browser
# 编辑 build/chromium-version.env
# CHROMIUM_TAG="133.0.6900.100"
# CHROMIUM_MILESTONE="133"
# CHROMIUM_PIN_DATE="2026-10-03"
# CHROMIUM_NEXT_REBASE="2026-12-26"
```

### 3. Reset Chromium 到新 tag

```bash
bash build/revert-patches.sh
cd /opt/chromium/src
git fetch --depth=1 origin "refs/tags/$CHROMIUM_TAG:refs/tags/$CHROMIUM_TAG"
git checkout -f "$CHROMIUM_TAG"
gclient sync --with_branch_heads --with_tags --nohooks
gclient runhooks
```

### 4. 尝试应用所有 patch

```bash
bash /root/byod-browser/build/apply-patches.sh
```

**预期结果**：部分 patch 会冲突（`git am` 停在冲突处）。

### 5. 逐个解决冲突

对每个冲突的 patch：

```bash
cd /opt/chromium/src
# 查看冲突
git status

# 编辑文件，解决冲突（保留 <<<<<<< 标记内应保留的内容）
vim <conflicted-file>

# 标记解决
git add <conflicted-file>

# 继续
git am --continue
```

若某个 patch 已经不再需要（因为上游改了行为使补丁变多余）：

```bash
git am --skip
# 然后删除 patches/ 里对应文件
```

### 6. 重新导出全部 patches

Rebase 后 patch 内容变了，必须重导出：

```bash
cd /opt/chromium/src
rm /root/byod-browser/patches/*.patch
git format-patch \
  --start-number=1 -N \
  "$CHROMIUM_TAG"..HEAD \
  -o /root/byod-browser/patches/
```

### 7. 编译 + smoke test

```bash
bash /root/byod-browser/build/build-linux.sh chrome
bash /root/byod-browser/build/smoke-test.sh
```

若编译失败：定位失败的 patch，修补后回到 Step 6 重新导出。

### 8. 跨平台验证

- Windows：在 Windows 机器上跑 `build-windows.ps1` + smoke test
- macOS：同上

### 9. 提交 rebase

```bash
cd /root/byod-browser
git add build/chromium-version.env patches/
git commit -m "chore: rebase to Chromium 133.0.6900.100"
git tag "rebase-133"
```

### 10. 通知 P1..P11 各 sub-plan 负责人

如果 rebase 触发了任一 sub-plan 的接口变更（例如 CDP scheme 名改了、gn arg 变了），更新相应文档。

## 应急：rebase 卡住怎么办

- 保留旧 tag 的 branch：始终能回滚
- 若某个 patch rebase 特别难，先 skip 那个 patch，其它先上线，单独修
- 记录本次 rebase 各 patch 的冲突情况到 `docs/development/rebase-history.md`（可选）

## 参考

- Chromium release schedule: <https://chromiumdash.appspot.com/schedule>
- Milestone dashboard: <https://chromiumdash.appspot.com/branches>
```

- [ ] **Step 2: Commit**

Run:
```bash
cd /root/byod-browser
git add docs/development/rebase-guide.md
git commit -m "docs: add 12-week rebase guide"
```

---

## Task 14: CI 配置（GitHub Actions 骨架）

**Files:**
- Create: `/root/byod-browser/ci/github-actions/build.yml`
- Create: `/root/byod-browser/ci/cache-keys.sh`

**说明：** 真正的 Chromium CI 编译在 GitHub Actions free tier 上不可行（Chromium 编译 2-4 小时，free tier 单 job 6 小时上限但很吃资源）。这里只搭"补丁 lint + 快速 metadata 检查"的 CI，实际全量编译走本地 or 自建 runner。

- [ ] **Step 1: 写 cache-keys.sh**

Write `/root/byod-browser/ci/cache-keys.sh`:

```bash
#!/usr/bin/env bash
# Emit cache keys used by CI.
# Usage: source cache-keys.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../build/chromium-version.env"

export CACHE_KEY_DEPOT_TOOLS="depot_tools-$(date +%Y%m)"
export CACHE_KEY_CHROMIUM_SRC="chromium-src-$CHROMIUM_TAG"
export CACHE_KEY_CHROMIUM_OUT_LINUX="chromium-out-linux-$CHROMIUM_TAG-$(cat /root/byod-browser/patches/*.patch 2>/dev/null | sha256sum | head -c 16)"
```

- [ ] **Step 2: 写 CI workflow**

Write `/root/byod-browser/ci/github-actions/build.yml`:

```yaml
name: Build & Lint

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

jobs:
  lint-patches:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Check patch file naming convention
        run: |
          set -e
          for p in patches/*.patch 2>/dev/null; do
            if [ ! -e "$p" ]; then break; fi
            base="$(basename "$p")"
            if ! [[ "$base" =~ ^[0-9]{4}-[a-z0-9-]+\.patch$ ]]; then
              echo "::error::patch $base violates naming convention (must be NNNN-slug.patch)"
              exit 1
            fi
          done
          echo "all patch names OK"

      - name: Check patch total line budget (≤ 3000)
        run: |
          set -e
          total=0
          for p in patches/*.patch 2>/dev/null; do
            if [ ! -e "$p" ]; then break; fi
            # count added lines (excluding metadata) as a rough proxy
            added=$(grep -c '^+' "$p" || true)
            total=$((total + added))
          done
          echo "total added lines: $total"
          if [ "$total" -gt 3000 ]; then
            echo "::error::patch budget exceeded ($total > 3000). See spec §3.1"
            exit 1
          fi

      - name: Verify chromium-version.env format
        run: |
          set -e
          source build/chromium-version.env
          echo "CHROMIUM_TAG=$CHROMIUM_TAG"
          if ! [[ "$CHROMIUM_TAG" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
            echo "::error::invalid CHROMIUM_TAG format"
            exit 1
          fi

      - name: Shellcheck build scripts
        run: |
          sudo apt-get update -qq && sudo apt-get install -y -qq shellcheck
          shellcheck build/*.sh

  # NOTE: full Chromium compile is not viable on GH free tier (2-4h, 30GB+ RAM)
  # We rely on developer laptops or self-hosted runners for that.
  # Future: add a self-hosted-runner job for full build here.
```

- [ ] **Step 3: 本地跑一遍 shellcheck 验证 scripts 都过**

Run:
```bash
which shellcheck || (apt-get update -qq && apt-get install -y -qq shellcheck)
shellcheck /root/byod-browser/build/*.sh
```
Expected: 无输出（无告警）；若有 shellcheck warning，回去修

- [ ] **Step 4: Commit**

Run:
```bash
cd /root/byod-browser
git add ci/
git commit -m "ci: add patch lint workflow (naming, line budget, shellcheck)"
```

---

## Task 15: P0 交付确认 checklist

**Files:**
- Create: `/root/byod-browser/docs/development/p0-acceptance.md`

**这是 P0 交付验收清单，跑通后 P0 才算完成。**

- [ ] **Step 1: 写验收 checklist**

Write `/root/byod-browser/docs/development/p0-acceptance.md`:

```markdown
# P0 交付验收 Checklist

按顺序执行；每一项通过打勾。全部通过则 P0 完成，可进入 P1。

## 环境类

- [ ] `bash build/install-depot-tools.sh` 成功
- [ ] `gclient --version` 输出版本号
- [ ] `bash build/fetch-chromium.sh` 成功（首次约 30-90 分钟）
- [ ] `ls /opt/chromium/src/chrome/browser | head` 有内容
- [ ] `git -C /opt/chromium/src describe --tags` 输出 pin 的 tag

## 构建类

- [ ] `bash build/build-linux.sh chrome` 成功（首编 2-4 小时）
- [ ] `bash build/smoke-test.sh` 全部通过
- [ ] 增量再编（改一行注释再编）10-30 分钟以内

## 补丁工作流类

- [ ] `bash build/apply-patches.sh`（空目录）返回 `no patches to apply`
- [ ] 按 Task 9 Step 5-9 走一遍测试 patch 的应用/撤销/删除，全通过
- [ ] `bash build/revert-patches.sh` 能把 src 还原到 clean tag

## CI 类

- [ ] GitHub Actions（或本地 `act`）跑通 `lint-patches` job

## 文档类

- [ ] `docs/development/build-environment.md` 存在
- [ ] `docs/development/patch-workflow.md` 存在
- [ ] `docs/development/rebase-guide.md` 存在
- [ ] `docs/development/p0-acceptance.md` 存在（本文件）
- [ ] 新开发者按 build-environment.md 独立跑通首编（找一个人试）

## 跨平台类（若资源允许）

- [ ] macOS 上跑通 `bash build/build-macos.sh chrome`
- [ ] Windows 上跑通 `powershell -File build\build-windows.ps1`

## 交付判定

以上全部打勾 → P0 完成，可以进入 [P1](../superpowers/plans/2026-07-11-byod-p1-chromium-patches.md)。

若跨平台类因资源不到位暂缓，可以先进 P1，但**必须在 P1 完成前补齐 Windows / macOS 首编**（否则 P1 的补丁没法验证跨平台是否受影响）。
```

- [ ] **Step 2: Commit**

Run:
```bash
cd /root/byod-browser
git add docs/development/p0-acceptance.md
git commit -m "docs: add P0 acceptance checklist"
```

- [ ] **Step 3: 更新 roadmap 标记 P0 完成**

Edit `/root/byod-browser/docs/superpowers/plans/2026-07-11-byod-roadmap.md`, change the P0 row status from `📝 待写` to `✅ 完成`。

- [ ] **Step 4: Final commit**

Run:
```bash
cd /root/byod-browser
git add docs/superpowers/plans/2026-07-11-byod-roadmap.md
git commit -m "docs: mark P0 complete in roadmap"
```

---

## P0 完成后

进入 [P1 · Chromium Fork 补丁集](2026-07-11-byod-p1-chromium-patches.md)（下一个 sub-plan，尚未编写；由 writing-plans 生成）。

**估计 P0 实际耗时：**
- 熟练开发者：2-3 天（含首编等待）
- 首次接触 Chromium 的开发者：1-2 周（含各种环境坑）

**P0 的 CI 只做 lint，没有全量编译**。CI 全量编译需要自建 runner（Chromium 官方开发者也用 Goma / RBE 分布式编译）；本项目暂不设。
