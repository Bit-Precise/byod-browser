# BYOD Exam Browser · Implementation Roadmap

> **索引文档**：本文件不是可执行 plan，而是 12 个 sub-plan 的总览、依赖图、执行顺序建议。每个 sub-plan 独立可执行。
>
> **关联 spec**：[`../specs/2026-07-11-byod-exam-browser-design.md`](../specs/2026-07-11-byod-exam-browser-design.md)

**Goal:** 把 spec v1 全部内容拆成 12 个独立可交付的 sub-plan，每个都能单独产出可测试的工件。

## Sub-Plans 一览

| # | Sub-plan | 文件 | 交付物 | 状态 |
|---|---|---|---|---|
| **P0** | Chromium 构建环境 & 补丁管理 | `2026-07-11-byod-p0-chromium-buildenv.md` | 能编译原版 Chromium stable；`git format-patch` 工作流；CI 缓存 | 📝 待写 |
| **P1** | Chromium Fork 补丁集（L2） | `2026-07-11-byod-p1-chromium-patches.md` | 15 项补丁全部合入；每项独立 build test | 📝 待写 |
| **P2** | IPC 信任根 & 心跳协议 | `2026-07-11-byod-p2-ipc-trust-root.md` | Browser↔Guardian 握手 + 5s 心跳 + challenge-response | 📝 待写 |
| **P3** | Guardian Windows (Rust) | `2026-07-11-byod-p3-guardian-windows.md` | ETW / RawInput / IME / VM / 反调试 / TPM | 📝 待写 |
| **P4** | Guardian macOS (Swift) | `2026-07-11-byod-p4-guardian-macos.md` | ES / CGEventTap / SE | 📝 待写 |
| **P5** | L3 策略与规则引擎 | `2026-07-11-byod-p5-policy-engine.md` | 白名单 / 快捷键 / 进程黑名单 / IME / 熔断决策器 | 📝 待写 |
| **P6** | L4 考试壳 UI | `2026-07-11-byod-p6-exam-shell.md` | OIDC device flow + 考试链接 + 生命周期 + 熔断 UI | 📝 待写 |
| **P7** | Browser Exam Key 协议 | `2026-07-11-byod-p7-browser-exam-key.md` | Session Key + 每请求 HMAC + Hardware Attestation | 📝 待写 |
| **P8** | Session Report 事件流 | `2026-07-11-byod-p8-session-report.md` | Merkle chain + SQLite 加密队列 + 断网补传 | 📝 待写 |
| **P9** | 策略热更协议（TUF-lite） | `2026-07-11-byod-p9-policy-hotupdate.md` | Root/policy 双层密钥 + 版本单调 + hard-coded 兜底 | 📝 待写 |
| **P10** | 安装包 + 自更新 | `2026-07-11-byod-p10-packaging.md` | NSIS + Squirrel (Win) / pkg + Sparkle (mac) | 📝 待写 |
| **P11** | Onboarding + PIPL 授权 | `2026-07-11-byod-p11-onboarding.md` | 首次运行流程、系统权限深链、分项授权、杀软加白教程 | 📝 待写 |

## 依赖图

```
        P0 (Chromium build env)
         │
         ▼
        P1 (15 patches)
         │
    ┌────┼────────────┬──────────────┐
    ▼    ▼            ▼              ▼
   P2   P5           P6             P7
    │    │            ▲              │
    ▼    ├──►P8       │              │
   P3    ├──►P9       │              │
   P4    └──────────► │              │
                      │              │
                      └──────────────┘
                             │
                             ▼
                            P10
                             │
                             ▼
                            P11
```

## 推荐执行顺序

### Phase A · 基础设施（阻塞其它一切）
1. **P0**：Chromium 构建环境 —— 2-4 周（含首编 2-4 小时 × N 次调试）
2. **P1**：15 项 Chromium 补丁 —— 6-10 周

**Phase A 里程碑**：能启动 fork Chromium，DevTools 已禁、chrome:// 只留白名单、Kiosk 强制生效、URL 白名单可拦截（策略硬编码）

### Phase B · 客户端骨架（可拿出内部 demo）
3. **P5**：L3 策略与规则引擎 —— 3-4 周
4. **P6**：L4 考试壳 UI —— 4-6 周
5. **P7**：Browser Exam Key 协议 —— 2-3 周（需要服务端配合）

**Phase B 里程碑**：能加载策略、OIDC device flow 登录、进入考试网页、每请求带 HMAC 签名 —— 一个"没有 Guardian"的最小可用版本

### Phase C · Guardian & 系统级监控（正式可用）
6. **P2**：IPC 信任根协议 —— 2-3 周
7. **P3** 或 **P4**（并行）：Guardian —— 各 6-8 周

**Phase C 里程碑**：完整 L1 + L2 能力，具备"上线正式考试"资格

### Phase D · 生产化（可对外发布）
8. **P8**：Session Report 事件流 —— 2-3 周
9. **P9**：策略热更协议 —— 3-4 周
10. **P10**：安装包 + 自更新 —— 3-4 周
11. **P11**：Onboarding + PIPL —— 2-3 周

**Phase D 里程碑**：v1.0 可发布

**总预计工作量**：约 40-60 周（个人）/ 20-30 周（3-5 人小团队）

## 状态标记

- 📝 待写 — sub-plan 尚未编写
- 🚧 待执行 — sub-plan 已写，未开工
- ⏳ 进行中
- ✅ 完成

## 关键约束（全局）

以下来自 spec §1.4 和 §2，**每个 sub-plan 都必须遵守**：

- **Chromium 补丁总量 ≤ 3000 行**（净增，不含 rebase 调整）
- **Chromium rebase 节奏 = stable channel 12 周对齐**
- **Chromium 补丁只做"删/禁"和"钩子暴露"，不写业务**
- **Guardian 语言分平台**：Windows = Rust；macOS = Swift + Objective-C++
- **业务代码全部在 L3/L4**，走可热更签名资源包
- **一批 hard-coded 最小信任规则永不可关闭**（见 spec §5.4.5）
- **无签名（v1）**：所有二进制暂不签名，架构预留签名钩子

## 每个 sub-plan 的通用要求

- **TDD**：每个任务先写失败测试，再实现，再验证通过
- **Frequent commits**：每个任务结束都 commit
- **文件路径全部绝对**（除 patch 里指 Chromium 相对路径外）
- **每步 2-5 分钟**：不写"实现 X" 这种大颗粒
- **无占位符**：不写 TODO / TBD / "similar to task N"

## Sub-plan 之间的接口契约

sub-plan 之间通过明确接口通信，写每个 sub-plan 时**必须列出 Consumes / Produces**：

- **P2 Produces**：IPC message 格式（Protocol Buffers）、握手 API、心跳超时事件
- **P3/P4 Produces**：Guardian 事件流（进程 / 键盘 / IME / VM / 显示器等）
- **P5 Produces**：策略引擎 API、熔断决策 API、事件规则 API
- **P6 Consumes**：策略引擎 API、Session Key、熔断信号
- **P7 Produces**：签名请求头、Hardware Attestation quote
- **P8 Consumes**：所有事件源；**Produces**：Session Report 上报格式
- **P9 Produces**：策略包格式、验证 API、hard-coded 规则常量
- **P10 Consumes**：所有产物；**Produces**：安装包格式、自更新协议
- **P11 Consumes**：所有 UI 元素、权限申请点

---

*本文档随 sub-plan 进度更新。*
