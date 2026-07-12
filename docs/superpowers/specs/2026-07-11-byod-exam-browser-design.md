# BYOD Exam Browser · 架构设计文档 (v1)

- **文档版本**：v1.0
- **日期**：2026-07-11
- **状态**：待评审
- **作者**：（待补）
- **目标读者**：核心研发团队、教务系统对接方、产品评审

---

## §1 项目定位与非目标

### 1.1 项目定位

一款基于 **Fork Chromium** 的 **Windows + macOS 双平台桌面考试浏览器**，用于**中国高校 / 机构在线考试**的 **BYOD（Bring Your Own Device）场景**。学生使用自带电脑安装本浏览器进入考试，服务端下发签名策略，浏览器强制隔离考试环境、监控异常行为、上报事件流。

对标 [Safe Exam Browser (SEB)](https://safeexambrowser.org/) —— 目标是在**中国场景下的可用性、跨平台一致性、防绕过深度**上超越 SEB。

### 1.2 目标场景

- 高校期末考试、职业培训 / 认证考试、企业内部考核
- 每场考试规模：数十到数千并发考生
- 考试类型（v1）：**纯 web 考试**（选择、填空、简答、代码编辑器等基于浏览器的题型）

### 1.3 明确的非目标（v1 不做）

- ❌ 移动端（iOS / Android / iPad）
- ❌ Linux 桌面
- ❌ 第三方受控应用启动（R Studio / Matlab / 计算器等）—— v2 roadmap
- ❌ 附加资源嵌入（PDF / 音频 / 视频参考资料）—— v2 roadmap
- ❌ 无障碍考生支持（TTS / 屏幕键盘 / 字号缩放）—— v2 roadmap
- ❌ AI 视频监考（摄像头 / 麦克风）—— 与第三方 SDK 集成，不自研
- ❌ 内核态屏幕捕获拦截
- ❌ 允许学生拒绝授权后仍能考试的降级模式（学生必须完全授权，否则不能考）
- ❌ 出题平台、教务后台、监考实时看板等**服务端产品**（本项目仅浏览器客户端 + 事件上报接口）

### 1.4 核心设计原则

1. **补丁最小化**：Chromium 补丁只做"删/禁"，不写业务；业务全部在可热更的 L3/L4
2. **权限分离**：安全能力放在独立 Guardian 进程，Browser 保持 Chromium sandbox 完整性
3. **信任根显式**：所有跨进程 / 客户端-服务端信任关系都有明确的密钥协商和 attest 过程
4. **五段式能力**：每个防御能力都定义 **能力 → 实现路径 → 已知边界 → 绕过检测 → 熔断动作**
5. **兜底不可覆盖**：热更策略可以调整业务规则，但一批 hard-coded 最小信任规则永远不可关闭

---

## §2 总体架构

### 2.1 进程模型

```
┌──────────────────────────────────────────────────────────────────┐
│                    Exam Browser Process Tree                      │
│                                                                   │
│   ┌────────────────────┐          ┌─────────────────────────┐    │
│   │  Browser Process   │◄───IPC──►│  Native Guardian Helper │    │
│   │  (Chromium main)   │  Mojo    │  (独立可执行, 高权限)   │    │
│   │                    │          │                          │    │
│   │ • UI / 窗口管理    │          │ • 进程枚举 (ETW)         │    │
│   │ • WebRequest 白名单│          │ • 键盘采集 (RawInput)    │    │
│   │ • 事件采集/上报    │          │ • 显示器 / 远程会话检测  │    │
│   │ • Kiosk 强制       │          │ • VM/反调试/软指纹       │    │
│   │ • Browser Exam Key │          │ • IME / TCC 检测         │    │
│   └─────────┬──────────┘          └────────────┬─────────────┘    │
│             │                                   │                  │
│             │ spawn (sandboxed, per-site)       │ heartbeat 5s     │
│             ▼                                   │ + challenges     │
│   ┌────────────────────┐                        │                  │
│   │ Renderer Processes │◄───────────────────────┘                  │
│   │ (per site instance)│  (Guardian 崩溃/失联 → Browser 熔断)     │
│   └────────────────────┘                                           │
└──────────────────────────────────────────────────────────────────┘
                               ▲
                               │ HTTPS + Browser Exam Key
                               ▼
               ┌──────────────────────────────────┐
               │  Exam Backend (已有)             │
               │  · 试题站                        │
               │  · OIDC IdP (device flow)        │
               │  · 监考事件收集 (本项目新增)     │
               │  · 策略下发 (本项目新增)         │
               │  · 硬件指纹绑定 (本项目新增)     │
               └──────────────────────────────────┘
```

### 2.2 为什么 Guardian 独立进程

三个理由：

1. **权限分离**：Chromium Browser 进程为了 sandbox 完整性不应做进程枚举 / 键盘钩子这类系统调用；这些放在独立 helper 里，Browser 进程保持"干净"，还能保住 Chromium 的沙箱安全模型
2. **互相守护**：Browser 挂了 Guardian 立刻上报"异常退出"事件；Guardian 挂了 Browser 立刻熔断（防学生用调试器 attach 后 kill 掉某一个进程绕过监控）
3. **代码签名与加固分离**：Guardian 需要用系统底层 API，编译签名策略不同；分开签名和分开加固（Win: `SetProcessMitigationPolicy` + CIG；mac: Hardened Runtime + hardened entitlements）更清爽

### 2.3 逻辑分层

```
┌─────────────────────────────────────────────────────────────┐
│  L4  Exam Session Layer                                     │
│      · 考试链接解析 · 配置校验 · 生命周期管理               │
│      · Session key 管理 · 交卷流程                          │
├─────────────────────────────────────────────────────────────┤
│  L3  Policy & Enforcement Layer                             │
│      · 白名单引擎 · 快捷键策略 · 进程黑名单 · IME 策略      │
│      · 事件规则引擎 · Session Report 编码 · 熔断决策        │
├─────────────────────────────────────────────────────────────┤
│  L2  Chromium Fork Layer  ← 编译期打入源码 (~2930 行)      │
│      · DevTools 入口移除 · scheme 白名单                    │
│      · 菜单裁剪 · 快捷键劫持 · Kiosk 强制                   │
│      · WebRequest hook 点 · Browser Exam Key 注入           │
│      · IPC 握手接口 · 事件采集钩子                          │
├─────────────────────────────────────────────────────────────┤
│  L1  Platform Abstraction Layer                             │
│      · Win / macOS 系统 API 屏蔽差异                        │
└─────────────────────────────────────────────────────────────┘
```

**核心原则**：**L2 静态锁死跟 Chromium stable 12 周节奏；L3/L4 是可热更的独立签名资源包**。日常业务迭代都在 L3/L4，避免频繁 rebase Chromium。

### 2.4 技术栈

| 层 | 语言 / 框架 | 说明 |
|---|---|---|
| Chromium 内核 | C++ (Chromium 源码 patch) | `depot_tools` + `gn`/`ninja` |
| Browser 进程业务扩展 (L2 内的钩子实现) | C++ (Chromium 组件) | 编入 chrome/browser 模块 |
| Guardian Helper | **Windows: Rust；macOS: Swift/Objective-C++** | 见 §3.2 说明 |
| 考试壳 UI (L4) | TypeScript + Preact / Vue | 运行在特权 WebUI `chrome://exam-shell` |
| 策略引擎 (L3) | C++ (与 Browser 同进程) + JSON 策略描述 | 策略包热更 |
| 服务端事件接收 | 沿用你已有后端；新增 HTTPS/gRPC 事件通道 | 本项目定义接口，不实现服务端 |
| 打包 | Win: MSIX；mac: pkg + notarization | |
| 自更新 | Win: Squirrel.Windows；mac: Sparkle | 均校验策略包签名 |

---

## §3 组件详细设计

### 3.1 Chromium Fork（L2）—— 补丁清单

**核心约束**：净增补丁总量 ≤ 3000 行；rebase 周期 12 周；只做"删/禁"和"钩子暴露"，不做业务。

| # | 模块 | 目的 | 关键修改点 | 行数 |
|---|---|---|---|---|
| 1 | 禁 DevTools 入口 | 快捷键 + 菜单 + scheme + 窗口创建 | `chrome/browser/devtools/devtools_window.cc`：`OpenDevTools*` 系列变 noop；快捷键 map 移除 F12 / Ctrl+Shift+I | ~150 |
| 2 | 禁 `--remote-debugging-port` | 命令行开关无效化 | `content/public/common/content_switches.cc` + `content/browser/devtools/devtools_http_handler.cc` 早退 | ~50 |
| 3 | 禁 chrome:// 除白名单 | 只留 `chrome://exam-shell` 和必需内部页 | `chrome/common/webui_url_constants.cc` + WebUI factory | ~200 |
| 4 | 禁 file:// / chrome-extension:// / view-source: / chrome-devtools:// | scheme handler 早退 | 各 scheme handler `HandleRequest` 直接 abort | ~100 |
| 5 | 禁 window.open + target=_blank | 强制主窗口内导航 | `content/browser/renderer_host/render_frame_host_impl.cc` 的 `CreateNewWindow` | ~80 |
| 6 | 禁右键 / 复制粘贴 / 打印 / 另存 / 拖拽 | context menu 裁剪 + edit command 拦截 | `chrome/browser/renderer_context_menu/render_view_context_menu.cc` + `edit_command_dispatcher` | ~200 |
| 7 | Kiosk 强制 | 无边框 / 置顶 / 锁尺寸 / 禁最小化 | `chrome/browser/ui/views/frame/` (Win) + Cocoa `NSWindow` subclass (mac) | ~300 |
| 8 | WebRequest 白名单 hook 点 | 暴露给 L3 策略层调用的接口 | 类似 `extensions/browser/api/web_request/` 机制 | ~250 |
| 9 | Browser Exam Key 请求头注入 | 每请求加签 | `services/network/network_context.cc` interceptor | ~200 |
| 10 | 禁开发者相关快捷键 | F12 / Ctrl+Shift+I / Ctrl+U / Ctrl+S / Ctrl+P | 快捷键 map | ~100 |
| 11 | 禁截屏快捷键（mac 特殊） | Cmd+Shift+3/4/5 拦截 | Cocoa event tap | ~150 |
| 12 | IPC 与 Guardian 握手接口 | 命名管道 / socketpair + 密钥协商 | 新增独立模块 `chrome/browser/exam/ipc/` | ~400 |
| 13 | Session / Config 生命周期 | 考试开始 / 结束的 Chromium 侧钩子 | 新增 `chrome/browser/exam/session/` | ~300 |
| 14 | 事件采集钩子 | blur / focus / resize / nav 事件桥接到 L3 | 主窗口 delegate | ~200 |
| 15 | 硬件指纹采集接口 | TPM (Win) / Secure Enclave (mac) 抽象 | 新增独立模块 `chrome/browser/exam/attestation/` | ~250 |
| **合计** | | | | **~2930** |

**编译**：`depot_tools` + `gn args`；启用 `is_official_build=true` + `symbol_level=1`（生产）；启用 CIG (Win) / Hardened Runtime (mac)。

**Rebase 策略**：跟 Chromium **stable channel + 12 周对齐**（非 4 周），每次约 1-2 周工作量。补丁用 `git format-patch` 管理，每个 patch 一个文件，rebase 冲突单独修。

### 3.2 Guardian Helper

**平台特化的语言选择**：

- **Windows Guardian**：**Rust**（`windows-rs` crate 覆盖够用，全局钩子 / ETW / 进程枚举稳定）
- **macOS Guardian**：**Swift + Objective-C++**（macOS 上 Endpoint Security / SecCode / Quartz Event Tap 的 Rust 绑定不成熟，unsafe FFI 反而抵消 Rust 优势）

**共享**：一份 IPC 协议定义（Protocol Buffers），两端各自实现。

**Guardian 职责**：

| 能力 | Win 实现 | mac 实现 |
|---|---|---|
| 进程枚举与监控 | ETW: `Microsoft-Windows-Kernel-Process` provider 事件驱动 | Endpoint Security Framework（若无签名 entitlement，退化到 `libproc` 轮询 30s） |
| 键盘采集（记录） | `RegisterRawInputDevices` | `CGEventTap` (passive) |
| 键盘拦截（如 Alt+Tab 阻断） | `SetWindowsHookEx WH_KEYBOARD_LL`（极简回调） | `CGEventTap` (active) + Accessibility 权限 |
| 显示器 / 外接屏检测 | WDDM 事件 + `EnumDisplayDevices` | `CGDisplayRegisterReconfigurationCallback` |
| 远程会话检测 | `WTSQuerySessionInformation` + 常见 VNC / TeamViewer 进程 | `IOKit` + 常见远程工具进程 |
| VM 检测 | CPUID hypervisor bit + BIOS 特征串 + 常见 VM 驱动文件 | `sysctl hw.model` + IOKit VM 特征 |
| 反调试（温和） | `QueryPerformanceCounter` 时间差 + `NtQueryInformationProcess ProcessDebugPort` | `ptrace(PT_DENY_ATTACH)` + `sysctl kinfo_proc` |
| 软指纹（TPM/SE 不可用时降级） | CPUID + `GetVolumeInformation` + `GetAdaptersAddresses` | `IOPlatformExpertDevice` + MAC 地址 |
| IME 检测 | `GetKeyboardLayout` + `ImmGetIMEFileName` | `TISCopyCurrentKeyboardInputSource` |
| TCC 权限反查 (mac only) | N/A | `AXIsProcessTrustedWithOptions` + `CGPreflightScreenCaptureAccess` |
| 心跳与 challenge 响应 | 见 §5.1 | 见 §5.1 |

### 3.3 L3 · 策略与规则引擎

**同 Browser 进程内的 C++ 组件**，业务逻辑与 Chromium 补丁解耦。

组件：

- **白名单引擎**：JSON 规则 → CIDR / 域名 / URL 模式匹配；双保险（WebRequest 层 + navigation throttle 层）
- **快捷键策略**：JSON 规则 → 快捷键黑名单；调用 L2 的钩子
- **进程黑名单**：JSON 规则 → 收到 Guardian ETW 事件时匹配 → 派消息给 Guardian 执行 `TerminateProcess`
- **IME 策略**：JSON 规则 → 收到 Guardian IME 事件时匹配 → 显示阻塞对话框要求切换
- **事件规则引擎**：JSON 规则 → 事件 pattern 匹配 → 触发 warning / burn action
- **Session Report 编码器**：见 §5.3
- **熔断决策器**：状态机，收敛所有熔断触发条件到统一出口

### 3.4 L4 · 考试壳 UI

**运行在特权 WebUI `chrome://exam-shell`**，非普通 web 内容，可以调用 Chromium 内部 API。

页面：

1. **登录页**：OIDC device flow（学生扫码 / 输码在手机确认），token 只在内存
2. **考试链接输入页**：粘贴考试链接 → 解析 token → 请求服务端下发签名策略包 → 校验 → 加载
3. **准备页**：环境自检（硬件指纹绑定、IME 检查、外接屏检查、进程扫描），有问题给出人可读的修复引导
4. **考试中容器**：iframe 加载白名单内的试题页，覆盖考试倒计时 / 事件提示条 / 熔断警告 UI
5. **异常页**：熔断 / 网络断开 / Guardian 崩溃时的 UI，含"联系监考老师"通道
6. **交卷完成页**：成功交卷确认，安全退出浏览器

---

## §4 完整能力清单（五段式）

按 review 建议，每个能力都定义：**能力 → 实现路径 → 已知边界 → 绕过检测 → 熔断动作**。

### 4.1 L1 · MVP 必做

#### 4.1.1 白名单 URL 拦截

- **能力**：只允许访问服务端下发策略里的域名 / URL 模式；违规导航立即 abort
- **实现路径**：
  - Chromium 补丁 #8：暴露 WebRequest hook 点给 L3
  - L3 白名单引擎：`Match(url) → allow | deny`
  - Chromium navigation throttle 层也调用同一引擎（双保险）
- **已知边界**：
  - 已加载页面内的 JS 通过 `fetch` / `XMLHttpRequest` 请求会被拦截
  - 但 WebSocket 需单独处理（`network::WebSocket` 层加拦截）
  - 单页应用 `history.pushState` 更新 URL 不触发 navigation，需在 renderer 侧 hook `DidCommitProvisionalLoad`
- **绕过检测**：
  - 若 Chromium 补丁未生效（哪个 hook 没插上），会出现"事件说 abort 但请求实际发出"—— 通过服务端接收到的 UA / IP 与本地事件对账检测
  - `chrome-extension://` / `about:blank` / `data:` scheme 直接跳绕过：补丁 #4 强制禁
- **熔断动作**：违规导航仅记录 + 展示警告条；短时间内 5 次违规触发 BURN

#### 4.1.2 强制 Kiosk 模式

- **能力**：无边框、置顶、全屏、锁定尺寸、禁最小化 / 关闭按钮 / 系统菜单
- **实现路径**：Chromium 补丁 #7
- **已知边界**：
  - Windows：`WS_POPUP` + `HWND_TOPMOST` + `SetForegroundWindow`；IME 悬浮窗仍会浮在其上（见 4.1.5）
  - macOS：`NSWindowStyleMaskBorderless` + `NSWindow.level = kCGMainMenuWindowLevel + 1` + `NSApplication.presentationOptions` 隐藏 Dock 和 Menu Bar
- **绕过检测**：Guardian 每 5s 报当前活跃窗口 title / 大小 / 是否 topmost，Browser 侧对账不一致熔断
- **熔断动作**：直接 BURN

#### 4.1.3 编译期禁 DevTools 与相关 scheme

- **能力**：F12 / Ctrl+Shift+I / `--remote-debugging-port` / `chrome-devtools://` / `chrome://inspect` 全部失效
- **实现路径**：Chromium 补丁 #1、#2、#3、#4、#10
- **已知边界**：
  - 保留 Chromium 内部 CDP 通信路径（Print Preview / Reader Mode 等内部依赖），只切用户可访问的入口
  - `chrome://exam-shell` 保留（考试壳自己用）
- **绕过检测**：
  - 学生用另一个未 patch 的 Chromium 假冒 UA → 由 Browser Exam Key 服务端强校验（见 §5.2）拒绝
  - 若 patch 编译时漏掉某个入口 → 由服务端接收异常事件（DevTools 打开的行为特征）检测
- **熔断动作**：不适用（这是编译期禁用，绕过路径落到其他检测）

#### 4.1.4 禁右键 / 复制粘贴 / 打印 / 另存为 / 拖拽 / 新窗口

- **能力**：菜单裁剪 + edit command 拦截 + window.open 强制主窗口内
- **实现路径**：Chromium 补丁 #5、#6
- **已知边界**：
  - JS 通过 clipboard API (`navigator.clipboard.writeText`) 主动写剪贴板可以工作 —— 由 L3 策略决定是否允许（默认允许，防止某些题型无法答题）
  - 拖拽仅禁"从考试页拖出内容"，考试页内部拖拽（如排序题）不受影响
- **绕过检测**：Guardian 监控剪贴板变更，非考试页触发的剪贴板写入记录事件
- **熔断动作**：仅记录，不 BURN

#### 4.1.5 窗口失焦 / 尺寸变化 / 截屏快捷键监控

- **能力**：blur / focus / resize 事件采集；截屏快捷键（Win PrintScreen / Win+Shift+S；mac Cmd+Shift+3/4/5）拦截
- **实现路径**：
  - blur / focus / resize：Chromium 补丁 #14 + L3 事件规则
  - 截屏快捷键：Chromium 补丁 #11 (mac) + Guardian `WH_KEYBOARD_LL` (Win)
- **已知边界**：
  - 失焦事件在多显示器 + 外接键盘场景可能有虚假触发（Win 焦点管理复杂）；用 debounce + 双重确认
  - Windows 上 `PrintScreen` 是硬件级键，某些键盘直接触发系统截屏，无法拦截（只能通过监控 clipboard 检测事后）
  - macOS 上 `Cmd+Shift+3/4/5` 可通过 Cocoa event tap 拦截，但需要 Accessibility 权限（学生未授权则失效）
- **绕过检测**：
  - macOS `screencapture` 命令行工具、Win PowerShell `Get-Clipboard` 等非快捷键截屏：见 4.1.6
- **熔断动作**：
  - 失焦：短时间累计超阈值（如 30s 内 5 次）→ CRITICAL；15 分钟内累计超 3 分钟离焦 → BURN
  - 尺寸变化：立即 BURN（因为 Kiosk 已锁定尺寸，变化必然异常）
  - 截屏快捷键：单次即 BURN

#### 4.1.6 OIDC device flow 认证

- **能力**：学生扫码 / 输码在手机 IdP 上确认，客户端不存密码，access token 仅内存
- **实现路径**：L4 考试壳内实现标准 OIDC device flow (RFC 8628)
- **已知边界**：
  - Refresh token 不落盘（考试结束即失效）
  - 学生若在手机上被中间人钓鱼 → 属于 IdP 层安全问题，不在本项目范围
- **绕过检测**：token 服务端 introspect 每请求校验
- **熔断动作**：token 失效 → 立即结束会话

#### 4.1.7 Browser Exam Key（每请求签名）

见 §5.2。

---

### 4.2 L2 · 正式上线必做

#### 4.2.1 进程黑名单

- **能力**：启动前 + 运行中扫描；发现 TeamViewer / AnyDesk / OBS / Zoom / 微信 / QQ / 钉钉等强制退出考试或杀掉
- **实现路径**：
  - Guardian ETW 订阅进程创建事件
  - 收到事件后计算 exe hash + 签名信息，与黑名单匹配（不用文件名，防学生 rename）
  - 匹配则 IPC 通知 Browser；Browser 显示对话框要求学生关闭该进程 → 学生关闭后自动继续；拒绝关闭则 BURN
  - 学生进考试前的准备页做一次全量扫描
- **已知边界**：
  - 黑名单需覆盖常见远程 / 屏幕录制 / IM 软件的多个版本 hash（服务端热更）
  - 便携版 exe（未安装、直接运行）也可通过 hash 匹配
  - 未知的新工具无法覆盖
- **绕过检测**：
  - 学生 patch 掉工具的 exe 让 hash 变化 → 通过签名信息匹配（大部分工具有签名）
  - 学生用 Wine / Crossover 里跑工具 → macOS Wine 进程能通过父进程名检测；Win 场景不适用
- **熔断动作**：见上

#### 4.2.2 禁系统热键

- **能力**：Win Alt+Tab / Win+D / Win+E / Win+R / Ctrl+Alt+Del；mac Cmd+Tab / Mission Control / Spotlight / Cmd+Q
- **实现路径**：
  - Win：Guardian `SetWindowsHookEx WH_KEYBOARD_LL` 拦截（无法拦 Ctrl+Alt+Del，那是系统级 SAS）
  - mac：Chromium 补丁 #11 + Guardian Cocoa event tap
- **已知边界**：
  - `Ctrl+Alt+Del` 无法拦截（Windows SAS 内核级）
  - mac `Cmd+Option+Esc` 强制退出对话框无法拦
  - 硬件键盘上的媒体键 / Fn 键组合平台无法拦
- **绕过检测**：
  - 学生按 `Ctrl+Alt+Del` 打开任务管理器 → Guardian 监控 taskmgr.exe 启动即记录事件
  - 学生按 mac `Cmd+Option+Esc` 强退 → 触发 Browser blur → 触发失焦熔断
- **熔断动作**：拦截即记录事件，不 BURN；关联的窗口切换（如 Alt+Tab 成功后 blur）由失焦熔断处理

#### 4.2.3 白名单 IME + 强制切换

- **能力**：只允许微软拼音 (Win) / Apple 拼音 (mac) / 系统英文；启动时检测，非白名单要求切换
- **实现路径**：
  - Guardian 启动时读活跃 IME
  - 非白名单则 IPC 通知 Browser 显示阻塞对话框（"请切换到微软拼音 / Apple 拼音后继续"）
  - 学生手动切换后 Guardian 检测到白名单 IME → 通知 Browser 关闭对话框自动继续
- **已知边界**：
  - 无法阻止学生在系统上装第三方 IME —— 只能不让用
  - 微软拼音的"自学习"功能理论上仍可能泄漏考题（虽无云端上传）—— v1 接受此风险
- **绕过检测**：运行期 Guardian 每 30s 采样活跃 IME，切回黑名单立即熔断
- **熔断动作**：启动期不切 = 不能进考试；运行期切回黑名单 → BURN

#### 4.2.4 禁多显示器 / 外接屏

- **能力**：启动前拒绝多显示器；运行中热插拔则熔断
- **实现路径**：
  - Guardian 启动时枚举显示器数量；> 1 则显示阻塞对话框
  - 运行中订阅显示器变更事件（Win: `WM_DISPLAYCHANGE`；mac: `CGDisplayRegisterReconfigurationCallback`）
- **已知边界**：
  - 无法区分"外接大屏"和"华硕 ZenBook Duo 硬件副屏" —— 都禁
  - HDMI 采集卡在系统层看是显示器，能被检测
- **绕过检测**：学生禁用副屏后进考试再启用 → 运行期检测热插拔即熔断
- **熔断动作**：启动期 = 不能进；运行期 = BURN

#### 4.2.5 禁 AirPlay / 投屏 / 远程会话

- **能力**：检测 RDP / VNC / SSH X 转发 / AirPlay 镜像 / Chrome Remote Desktop
- **实现路径**：Guardian 定期检查
  - Win: `WTSQuerySessionInformation` (`WTSClientProtocolType` = 2 表示 RDP) + 进程黑名单（TeamViewer 等）
  - mac: `NSScreen` API 检测镜像 + 进程黑名单
- **已知边界**：新型远程工具或修改版可能漏检 → 依赖黑名单热更
- **绕过检测**：定期扫描 30s 一次
- **熔断动作**：立即 BURN

#### 4.2.6 网络白名单（出站流量域名限制，可选）

- **能力**：整个 Browser 进程的出站 HTTP(S) 流量只允许白名单域名
- **实现路径**：Chromium 补丁 #8 已覆盖（WebRequest 层拦截）；DNS 层可选（若做则需要注入 mac Network Extension / Win WFP driver，v1 不做）
- **已知边界**：
  - 仅拦 Chromium 内的流量；系统其他进程流量不管
  - 学生若在其他浏览器 / 应用里查资料 → 靠进程黑名单 + 失焦事件覆盖
- **熔断动作**：违规请求 abort + 记录

#### 4.2.7 考试快照 Session Report

见 §5.3。

#### 4.2.8 异常自动交卷 / 熔断

- **能力**：触发 BURN 事件后暂停考试 + 显示监考老师联系方式 + 服务端锁定；教师端可"允许恢复"或"强制交卷"
- **实现路径**：L3 熔断决策器 → L4 显示熔断 UI + 上报服务端 → 教师端决策 → 服务端下发 resume / submit 命令
- **已知边界**：断网时无法与教师端联动 → 达阈值自动交卷
- **熔断动作**：见上

---

### 4.3 L3 · 精选加固

#### 4.3.1 VM 检测

- **能力**：检测学生是否在 VMWare / VirtualBox / Parallels / QEMU 里跑本浏览器
- **实现路径**：Guardian
  - CPUID hypervisor bit（bit 31 of CPUID.1:ECX）
  - Win: BIOS 特征串 `SystemManufacturer` == "VMware, Inc." 等；VM 驱动文件 `\\.\HGFS`, `\\.\vmci`
  - mac: `sysctl hw.model` == "VMware..." 或类似；Rosetta 检测
- **已知边界**：定制 VM 可以伪造 CPUID bit；VM breakout 后表现如物理机
- **绕过检测**：检测多个信号（不只看 CPUID），任一命中即视为 VM
- **熔断动作**：启动期 = 不能进；运行期检测到（罕见）= BURN

#### 4.3.2 反调试（温和）

- **能力**：检测有人 attach 调试器到 Browser / Guardian
- **实现路径**：
  - Win: `NtQueryInformationProcess` `ProcessDebugPort` != 0；`QueryPerformanceCounter` 时间差检测单步
  - mac: `ptrace(PT_DENY_ATTACH)` 拒绝被 attach；`sysctl kinfo_proc` 查 `P_TRACED` flag
  - 不用 `IsDebuggerPresent`（明牌，易被 hook）
- **已知边界**：
  - `PT_DENY_ATTACH` 在 macOS 11+ 已弱化，仍可被内核调试器绕过
  - 时间差检测有假阳性（CPU 调度、GC）—— 阈值宽松
- **绕过检测**：多重检测组合
- **熔断动作**：立即 BURN

#### 4.3.3 代码签名自检

- **能力**：启动时校验自身可执行文件的**hash 白名单**（因无签名，退化到 hash 比对）
- **实现路径**：
  - Browser 启动时算 Guardian exe 的 SHA256，与编译期嵌入 Browser 的 hash 白名单比对
  - Guardian 启动后算 Browser exe 的 SHA256，与自己嵌入的 hash 白名单比对
  - 双向不匹配任一方立退
- **已知边界**：
  - 攻击者若同时替换 Browser 和 Guardian 两个 exe 并生成新的匹配 hash 白名单 —— 但那需要重新编译整个项目
  - v1.x 加上 EV 签名后升级到"签名校验"（更强）
- **绕过检测**：见上
- **熔断动作**：立退

#### 4.3.4 硬件指纹（TPM / Secure Enclave 优先，软指纹降级）

见 §5.2.3。

---

## §5 事件与协议

### 5.1 IPC 信任根（Browser ↔ Guardian）

**这是整个架构的信任根，最关键的一节。**

#### 5.1.1 密钥协商

**Windows**：
1. Browser 主进程创建匿名 pipe：`CreatePipe(&hRead, &hWrite, &sa, 0)`，`sa.bInheritHandle = TRUE`
2. `BCryptGenRandom` 生成 256-bit `ipc_key`
3. `CreateProcess` fork Guardian，通过 `STARTUPINFOEX.lpAttributeList` 的 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 传 `hRead` 给子进程
4. Browser 通过 pipe 写 `ipc_key` (32 字节)；Guardian 从 pipe 读；pipe 关闭
5. **`ipc_key` 从未落盘、从未上命令行、从未上环境变量**

**macOS**：
1. `socketpair(AF_UNIX, SOCK_STREAM, 0, sv)` 创建一对连接的 socket
2. `posix_spawn` fork Guardian，通过 `posix_spawn_file_actions_adddup2` 把 `sv[1]` 传给子进程作为 fd 3
3. 通过 socketpair 传 `ipc_key`

#### 5.1.2 双向身份 attest

**Windows**：
- Guardian：`NtQueryInformationProcess(ProcessBasicInformation)` 拿 parent PID → `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `QueryFullProcessImageName` 拿 parent exe 路径 → 校验 = 自身路径同目录下已知 Browser 文件名 + hash 白名单
- Browser：拿 Guardian 的 PID 做同样校验
- 校验失败任一方立即 `ExitProcess(1)`

**macOS**：
- Guardian：`SecCodeCopyGuestWithAttributes` 拿对端 SecCode → `SecCodeCheckValidity` 校验（无签名场景校验可执行体 hash）
- Browser：对 Guardian 做同样校验
- 校验失败立退

#### 5.1.3 主通道

- Windows：命名管道 `\\.\pipe\exam_ipc_<random>`，双向读写
- macOS：继续用 socketpair
- 每条消息：`{version, seq, type, payload, hmac(ipc_key, seq||type||payload)}`；`seq` 单调递增防重放

#### 5.1.4 心跳与熔断

- **周期**：5s
- **时钟源**：Win `QueryUnbiasedInterruptTime` / mac `mach_absolute_time`（单调时钟，不受 sleep / NTP / 时区变更影响）
- **状态机**：
  ```
  HEALTHY  ──(5s 内收到心跳)──►  HEALTHY
  HEALTHY  ──(>8s 未收到)──►  DEGRADED   (仅记录事件)
  DEGRADED ──(收到心跳)──►    HEALTHY
  DEGRADED ──(连续 3 次超时, ~24s)──►  CRITICAL   (UI 显示"通信异常"警示)
  CRITICAL ──(收到心跳)──►    HEALTHY   (清除警示)
  CRITICAL ──(连续 6 次超时, ~48s)──►  BURN      (触发熔断)
  任意状态 ──(wall clock 未恢复 > 5min)──►  BURN
  ```
- **系统事件订阅**：Win `WM_POWERBROADCAST` (`PBT_APMSUSPEND` / `PBT_APMRESUMEAUTOMATIC`) / mac `NSWorkspaceWillSleepNotification` + `NSWorkspaceDidWakeNotification`；触发时暂停计时并 reset
- **重连**：不重启 Guardian；一旦 BURN 就熔断整个考试

#### 5.1.5 反 mock 的周期 challenge-response

Browser 每 30s 随机发一个 challenge：

| Challenge | 期望响应 |
|---|---|
| "报告当前活跃 IME 名称" | Guardian 用 `TISCopyCurrentKeyboardInputSource` / `GetKeyboardLayout` 采集 |
| "报告近 30s 内新起进程列表" | Guardian 用 ETW 事件缓存返回 |
| "报告 TPM/SE attestation quote (nonce=X)" | Guardian 调硬件 API 返回签名 quote |
| "报告当前显示器数量与分辨率" | Guardian 用 `EnumDisplayDevices` / `CGGetActiveDisplayList` |

响应超时 (>3s) 或格式错误 → 立即 BURN。fake Guardian 因为无法采集真实系统数据（或无 hardware attestation key），无法伪造正确响应。

### 5.2 Browser Exam Key（每请求签名 + 硬件指纹）

#### 5.2.1 Session Key

- 服务端下发考试配置时同时下发 256-bit `session_key`（用学生 OIDC token 换取，一次性）
- 只存 Browser 内存，绝不落盘
- 考试结束 / 超时清除

#### 5.2.2 请求头签名

Browser 向白名单域名发的每个 HTTP 请求都注入以下头（Chromium 补丁 #9）：

```
X-Exam-Auth: HMAC-SHA256(session_key, method || url_path || iso_timestamp || nonce)
X-Exam-Timestamp: 2026-07-11T14:30:22Z
X-Exam-Nonce: <base64 random 16 bytes>
X-Exam-Client-Fingerprint: <hardware_attested_id>
```

**服务端每个请求校验**：
- 签名正确
- Timestamp 在 ±60s 窗口内
- Nonce 在滑动窗口（近 5 分钟）内未复用

#### 5.2.3 Hardware Attestation

**Windows (TPM 2.0)**：
- 首次启动请求 TPM 生成 Attestation Key (AK)，由 Endorsement Key (EK) 认证
- Guardian 通过 `Tbs*` API 调 TPM；AK 的公钥上传服务端；私钥留 TPM 内
- 每次上报的 `Client-Fingerprint` = AK 签名的 `(machine_id + timestamp)` 摘要
- 服务端首次绑定 (student_id, AK public)；换机需教师端解绑

**macOS (Secure Enclave)**：
- `SecKeyCreateRandomKey` 生成 EC key，存 Secure Enclave（`kSecAttrTokenIDSecureEnclave`）
- 公钥上传服务端；私钥无法导出
- `Client-Fingerprint` = SE key 签名的 `(system_id + timestamp)` 摘要

**降级（无 TPM / 无 SE 老机器）**：
- 用 CPU serial + 主板 UUID + 网卡 MAC 合成 SHA256 = 软指纹
- 服务端标记该场次 `attestation_level = "soft"`；触发规则更严格

#### 5.2.4 交卷额外签名

- 每次提交答题（POST /submit）时，在 body 里附加：
  ```
  submit_signature: HardwareKey.sign(SHA256(payload))
  ```
- 服务端强校验；防止学生用 curl 绕过浏览器直接提交伪造答案

### 5.3 Session Report 事件流

#### 5.3.1 事件结构

```json
{
  "seq": 1234,
  "timestamp_monotonic": 987654321,
  "timestamp_wall": "2026-07-11T14:30:22.123Z",
  "prev_hash": "sha256:abc...",
  "event_type": "window.blur",
  "severity": "warning",
  "payload": { ... },
  "hmac": "hmac_sha256(session_key, seq || prev_hash || event_type || payload)"
}
```

- `seq` 单调递增（Browser 侧和 Guardian 侧各自维护序列，服务端合并时按 (source, seq) 排序）
- `prev_hash` = 前一条事件 `hmac` 的 hash，构成 **Merkle chain**
- `hmac` 用 session_key 签名，防修改
- `timestamp_wall` 仅参考，服务端接收时打权威时间戳

#### 5.3.2 本地缓存

- SQLite 文件（`%APPDATA%/ExamBrowser/session.db` / `~/Library/Application Support/ExamBrowser/session.db`）
- 加密：AES-256-GCM，key = HKDF(session_key, "session_report_encryption")
- 每条事件插入即 `fsync` 保证落盘
- 考试结束清理

#### 5.3.3 上报

- 每 5s 批量 flush 到服务端 `/api/exam/events`（HTTP POST，带 Browser Exam Key 头）
- 失败指数退避：1s → 2s → 4s → 8s → 16s → 30s (cap)
- 断网期间事件继续入本地队列，恢复后按 seq 顺序补传

#### 5.3.4 服务端校验

- 校验每条事件的 hmac（用服务端保存的 session_key 副本）
- 校验 prev_hash 链完整
- 检测 seq gap：缺失即人工审核标红
- Wall clock 与服务端时间偏差 > 5 分钟：标记异常

#### 5.3.5 断网熔断

- Wall clock 超 5 分钟未成功上报 → BURN
- v1.x 根据实际用户网络数据调整阈值

### 5.4 策略包热更协议

#### 5.4.1 双层密钥

- **Root key**：Ed25519，离线，2 年轮换；只用于签"授权信任的策略签名 key 声明"
- **Policy signing key**：Ed25519，日常，定期轮换（比如每 3 个月）；用于签实际策略包
- Browser 二进制**编译期内嵌 root 公钥**

#### 5.4.2 策略包结构

```
policy-package.tar.gz
├── manifest.json
│     {
│       "version": 20260711001,       // uint64, 服务端全局单调递增
│       "signed_at": "2026-07-11T00:00:00Z",
│       "server_nonce": "<random>",
│       "policy_signing_key_id": "psk-2026-01",
│       "policy_signing_key_authorization": "<signature by root key>",
│       "content_hash": "sha256:..."
│     }
├── manifest.sig       // signature by policy_signing_key
├── policy/
│    ├── whitelist_domains.json
│    ├── prohibited_processes.json
│    ├── ime_whitelist.json
│    ├── hotkey_rules.json
│    └── ...
└── shell/
     └── (考试壳 UI 静态资源, 若有更新)
```

#### 5.4.3 客户端验证流程

1. 拉取 `policy-package.tar.gz`
2. 解出 `manifest.json` + `manifest.sig` + `policy_signing_key_authorization`
3. 用编译期内嵌的 root 公钥验证 `policy_signing_key_authorization` → 得到当前受信 policy_signing_key 公钥
4. 用该公钥验证 `manifest.sig`
5. 验证 `manifest.version >= last_seen_version`（本地持久化，Win DPAPI / mac Keychain 保护）
6. 验证 `content_hash` = SHA256(policy/ 目录 tar)
7. 全部通过 → 更新 `last_seen_version` → 应用策略
8. 任何一步失败 → 拒绝并上报 `policy.rejected` 事件

#### 5.4.4 拉取时机

- Browser 启动同步拉一次
- 考试进行中**不轮询**（避免中途策略变更）

#### 5.4.5 Hard-coded 最小信任规则（热更覆盖不了）

编译期锁死在 L2 补丁中：

- `chrome://` scheme 除 `chrome://exam-shell` 外全禁
- `file://` 永远禁
- 本机 IP (127.0.0.1, ::1, 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16) 永远禁
- 白名单不能包含通配所有域名的规则（如 `*` / `*.com`）
- Guardian 心跳熔断策略不可关
- Chromium 补丁清单里所有"禁"永远生效

---

## §6 分发、打包、自更新

### 6.1 打包

**Windows**：
- 主发行格式：`.exe` 安装包（NSIS 或 Inno Setup）
- 探索 MSIX：v1.x 视情况
- 组件：Browser exe + Guardian exe + Chromium 资源 + 考试壳 UI 静态资源
- PE 元数据完整填写（公司名、产品名、版权、描述、图标）
- 无签名（v1）；v1.x 加 OV 签名；v2 加 EV 签名并启动杀软报白

**macOS**：
- 主发行格式：`.pkg`（notarized）
- Bundle 结构：`ExamBrowser.app/Contents/MacOS/{Browser, Guardian}`
- 无签名（v1）会有 Gatekeeper 阻止 → onboarding 引导学生"控制面板 > 安全性 > 仍要打开"
- v1.x 争取开发者账号后走 notarization

### 6.2 自更新

**Windows**：Squirrel.Windows
**macOS**：Sparkle

**签名**：
- 更新包用 policy signing key 的一个专用子 key（`update_signing_key`）签名
- 客户端用编译期内嵌的 root key 验证 update_signing_key 的授权
- 版本单调（同 5.4.3）

**更新时机**：非考试期间；考试中检测到有更新只提示不下载

### 6.3 Onboarding

学生第一次运行浏览器时：

1. **欢迎页 + 无签名告知**
   > "本浏览器目前未取得数字签名。您的杀软（360 / 火绒 / 腾讯管家 / Windows Defender）可能提示'未知发布者'或阻止启动。这是正常现象，请按下面的指南添加信任。"
2. **杀软加白教程**：分别提供 360 / 火绒 / 腾讯管家 / Windows Defender / macOS Gatekeeper 的图文教程
3. **权限授权分项确认**（PIPL）：
   - 逐项列出：采集什么 / 存储位置 / 加密方式 / 保留期限 / 使用目的 / 撤回通道
   - 学生逐条勾选（不能一次性勾全部）
   - 授权记录 hash 上服务端做证据
4. **系统权限申请** (mac)：
   - 分步引导授权辅助功能 / 输入监视 / 屏幕录制
   - 用 `x-apple.systempreferences:` URL scheme 直接跳对应设置面板
5. **环境自检**：显示器数量、IME、进程扫描、TPM/SE 可用性；不通过给具体修复引导

### 6.4 考前 24 小时"环境自检"链接

参考 SEB Server：教师端可提前 24 小时开放一个"预检链接"，学生用同一台机器跑一遍，提前发现杀软冲突、IME 问题、外接屏问题；预检结果绑定学生 ID 上服务端，考前 24 小时内通过预检的学生正式开考时可跳过部分检查。

---

## §7 已知边界与不做事项

### 7.1 承认的绕过路径（技术不能全解）

| 攻击 | 现状 |
|---|---|
| 硬件级键盘拦截（USB pass-through 硬件） | 软件层不可探；靠视频监考 |
| 副机（HDMI 采集卡 / 手机拍屏） | 靠视频监考 |
| 隔壁人在旁边指导 | 靠视频监考 |
| 学生用未 patch 的 Chromium 假冒 UA | Browser Exam Key 服务端校验拒绝 |
| 学生同时替换 Browser + Guardian 两个 exe | hash 白名单校验；EV 签名后升级 |
| 系统时间篡改 | 服务端权威时间戳；短时效 token 服务端校验 |
| Ctrl+Alt+Del 打开任务管理器 | 无法拦，但监控 taskmgr.exe 记录事件 |
| 手机 IdP 上被中间人钓鱼 | 属 IdP 安全，不在本项目范围 |
| Windows PrintScreen 硬件键触发系统截屏 | 无法拦；通过 clipboard 变更监控事后 |
| macOS `screencapture` 命令行 | Guardian 进程黑名单覆盖常见截屏工具 |

### 7.2 v1 不做（明确 defer）

- ❌ 移动端 / iPad
- ❌ Linux 桌面
- ❌ 第三方受控应用启动（v2）
- ❌ 附加资源嵌入（v2）
- ❌ 无障碍考生支持（v2）
- ❌ AI 视频监考 / 摄像头麦克风（v2 或与第三方 SDK 集成）
- ❌ 内核态屏幕捕获拦截
- ❌ 学生拒绝授权仍能考试的降级模式
- ❌ 教师端后台 / 监考实时看板 / 出题平台（不在本项目范围）
- ❌ 允许"每场考试单独定制策略"细粒度（v1 使用统一策略包 + 少量场次级 override）

---

## §8 v2 Roadmap

按优先级排列，非承诺：

1. **AT 特殊考生放行策略**：服务端策略下发支持按学生 override `allow_at`，覆盖视障 / 听障 / 肢体障碍考生场景
2. **第三方受控应用启动**：白名单启动 R Studio / Matlab / 计算器 / Excel 等；Guardian 监控子进程 + 阻止其派生新进程 / 开网络
3. **附加资源嵌入**：受控嵌入 PDF / 音频 / 视频；服务端签名 + 域内加载
4. **无障碍支持**：内置 TTS 朗读题目 / 屏幕键盘 / 字号缩放 / 高对比度
5. **EV 签名 + 杀软报白**：申请到营业执照后启动 EV 证书；同步启动 360 / 火绒 / 腾讯管家的报白流程
6. **移动端**：iOS (WKWebView + Guided Access) + Android (Kiosk / Lock Task Mode)
7. **AI 监考集成**：与声网 / 云信等 SDK 集成，加人脸检测 / 多脸告警 / 离席识别
8. **教务后台产品化**：教师端 / 监考实时看板 / 出题平台

---

## §9 合规与法律

### 9.1 PIPL 采集分级

**匿名元数据**（常规上传）：时间戳、事件类型、进程名 hash、窗口 title hash、显示器数量、IME 名字

**含内容原始数据**（仅本地加密临时存储 + 争议时按学生再次同意上传）：截图、按键 keycode 内容、剪贴板内容
- 加密：AES-256-GCM，key 派生自 session_key
- 保留：**T+7 自动删除**
- 上传：仅在教务端发起争议时，弹窗要求学生再次同意后上传

### 9.2 授权文案

Onboarding 时 PIPL 分项授权（见 6.3.3），必须包含：
- 处理者身份（本浏览器运营方）
- 处理目的（考试安全监控）
- 处理方式（本地记录 + 服务端上报）
- 处理种类（列出每一类）
- 保留期限
- 学生权利（查询 / 更正 / 删除 / 撤回）与行使方式

### 9.3 学生拒绝授权 = 不能考

**产品定位明确**：学生必须完全授权（含 mac 屏幕录制、辅助功能等 TCC 权限）才能开考。onboarding 时明确告知，不做技术降级。若学生因此无法考试 → 联系教务走线下替代考试流程（教务责任，非本浏览器范围）。

### 9.4 数据保留

- Session Report（服务端）：按学校要求保留（一般 1-2 年，用于成绩争议）
- 本地缓存（学生机器）：考试结束清理；争议数据 T+7 自动删除
- 硬件指纹（服务端）：与 student_id 绑定，学生毕业 / 注销时按 PIPL 删除

---

## §10 与 SEB 的详细对比

| 维度 | Safe Exam Browser | 本项目 |
|---|---|---|
| 平台覆盖 | Win / mac / iOS / iPadOS | Win / mac (v1) |
| Windows 内核 | Chromium (via CefSharp) | Fork Chromium |
| macOS 内核 | **WebKit** | Fork Chromium **（同一份，跨平台一致）** |
| DevTools 禁用 | 运行期拦截 | **编译期删除入口** |
| 语言 | Windows: C# / .NET；macOS: Objective-C / Swift | Win Guardian: Rust；mac Guardian: Swift/Objective-C++；Browser: C++ |
| 配置分发 | .seb 加密 XML 文件 | **签名短时效链接 + 策略包热更** |
| 密钥机制 | Config Key + Browser Exam Key | Session Key + Browser Exam Key + Hardware Attestation |
| 硬件指纹 | SEB Server 支持 | **TPM / Secure Enclave + 软指纹降级** |
| 事件采集 | SEB Server 集成时才有 | **一等公民，Merkle chain + 断网补传** |
| 第三方应用 | ✅ Prohibited + Permitted | ❌ (v2) |
| 附加资源 | ✅ Additional Resources | ❌ (v2) |
| 无障碍 | 部分 | ❌ (v2) |
| 生态与合规 | 15+ 年成熟，多国机构认可 | 从 0 起步 |
| LMS 集成 | Moodle / OpenOlat / Ilias 深度 | 需自行对接 |

### 10.1 我们的架构性优势

1. **跨平台内核统一** —— 消除 SEB Win/mac 引擎分裂带来的题目渲染不一致
2. **编译期彻底移除敏感能力** —— 相比 SEB 的"运行期禁用"，防绕过更深
3. **配置通过签名短时效链接** —— 相比 .seb 文件更灵活、可撤销、按学生定制
4. **双进程互相守护** —— 相比 SEB 一体化架构，攻击面更小、单点被 kill 也能感知
5. **策略层热更** —— 无需重发浏览器即可更新黑名单 / 规则
6. **事件流一等公民** —— 从 day 0 就是核心组件，为后期 AI 行为分析留接口

### 10.2 SEB 相较我们的优势

1. 成熟度：15 年生产验证
2. 平台覆盖：多一个 iOS / iPad
3. 生态：LMS 深度集成 + 教务系统适配
4. 合规：多国教育机构官方认可
5. 无障碍：更成熟
6. 第三方应用 / 附加资源：已具备

**结论**：我们的优势是**架构选择更现代、跨平台一致性更好、防绕过在编译层更彻底、迭代节奏更灵活**；SEB 的优势是**成熟度、生态、合规**。**目标不是完全替代 SEB，而是在中国场景下做更好的选择**。

---

## §11 风险与开放问题

| # | 风险 | 严重度 | 缓解 |
|---|---|---|---|
| 1 | 无 EV 签名 + 无报白，杀软误报率 5-15% | 🔴 高 | 强 onboarding 告知 + 详细自救教程 + 考前预检 + 服务端"临时宽松模式"通道 |
| 2 | Fork Chromium 每 12 周 rebase 工作量 | 🟠 高 | 补丁总量 ≤ 3000 行 + `git format-patch` 管理 + CI 自动化 build |
| 3 | 学生拒绝授权 = 不能考，可能引发教务投诉 | 🟠 中 | 明确产品定位；提供教务线下替代考试方案 |
| 4 | macOS TCC 权限 UX 灾难（学生要授权 3-5 项） | 🟠 中 | 分步 onboarding + 视频 + 深链跳设置面板 |
| 5 | 输入法白名单强制切换可能被学生绕过 | 🟠 中 | 运行期持续监控 IME + 切回黑名单立即熔断 |
| 6 | 硬件级键盘 / 副机作弊无法防 | 🟡 中 | 承认边界；靠视频监考补 |
| 7 | 中国网络不稳定导致断网熔断误伤 | 🟡 中 | 5 分钟阈值宽松；v1.x 根据数据调整 |
| 8 | 服务端接口本项目不实现，依赖已有后端方进度 | 🟠 高 | 早期与后端方确认接口契约（本 spec §5） |

**开放问题**（待与团队 / 教务确认）：

- Q1：策略包热更服务的部署位置（是否复用已有后端）？
- Q2：Session Report 的服务端存储规模预估（每场考试每学生约 KB 级）？
- Q3：熔断后教师端的"允许恢复 / 强制交卷"UI 由谁实现？
- Q4：v1 是否需要"多考生同一台机器"支持（家庭场景）？
- Q5：Chromium 补丁的开源许可如何处理（Chromium 是 BSD-3，我们的补丁也应开源？）

---

## §12 附录

### 12.1 术语表

- **BYOD**：Bring Your Own Device，考生自带设备
- **SEB**：Safe Exam Browser
- **CEF**：Chromium Embedded Framework
- **CDP**：Chrome DevTools Protocol
- **OIDC device flow**：OpenID Connect 设备授权流程（RFC 8628）
- **BURN**：本文档中定义的最高级熔断状态，触发暂停考试 + 服务端锁定
- **AT**：Assistive Technology，辅助技术（屏幕阅读器等）
- **TCC**：macOS Transparency, Consent, and Control（隐私权限管理框架）
- **ETW**：Event Tracing for Windows
- **TUF**：The Update Framework（安全更新框架规范）

### 12.2 参考资料

- Safe Exam Browser: https://safeexambrowser.org/
- Safe Exam Browser Source: https://github.com/SafeExamBrowser
- Chromium docs: https://www.chromium.org/developers/
- OIDC Device Flow RFC 8628: https://www.rfc-editor.org/rfc/rfc8628
- The Update Framework: https://theupdateframework.io/
- 《中华人民共和国个人信息保护法》
- 《中华人民共和国数据安全法》

---

*文档结束*
