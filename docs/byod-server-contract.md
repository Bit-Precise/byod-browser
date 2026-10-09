# BYOD 中台与浏览器联调契约

这份契约把浏览器的考试生命周期和中台职责分开。浏览器负责展示状态、验证策略签名和执行限制；中台负责 OIDC 认证、会话状态、策略发布和受控的 L4 转发。源站 TLS 在浏览器与源站之间端到端保持，中台不做 TLS termination，也不向源站注入身份 header。

## 请求时序

```mermaid
sequenceDiagram
  participant B as grips://exam WebUI
  participant M as BYOD 中台
  participant I as OIDC IdP
  participant E as 考试源站
  B->>I: 当前标签页打开 grips://exam/ 后登录
  I-->>M: OIDC callback，建立用户 session
  B->>M: GET /v1/exams/available（OIDC cookie）
  M-->>B: assigned exam_id + schedule + publication state
  B->>M: GET /{exam_id}/.well-known/byod-configuration
  M-->>B: origin + OIDC endpoints + signed policy
  B->>M: POST /v1/sessions {exam_id, browser_nonce}
  M-->>B: session_id + browser_token + authorization_url
  B->>I: 当前标签页打开 authorization_url
  I-->>M: GET /oidc/callback?code&state
  B->>M: GET /v1/sessions/{id} (Bearer browser_token)
  M-->>B: authenticated
  B->>M: POST /v1/sessions/{id}/start (only after starts_at)
  B->>M: POST /v1/sessions/{id}/tunnel-ticket
  M-->>B: ticket + endpoint_id（考试窗口内有效）
  B->>M: HTTPS XHTTP GET + POST（VLESS UUID、目标、源站 TLS）
  M->>E: 原样转发 TLS ClientHello / application bytes
  E-->>M: 原样返回 TLS bytes
  M-->>B: 原样返回 TLS bytes
  B->>M: POST /v1/exams/{exam}/complete
  M-->>B: ended; completion is durable and re-entry is denied
```

## 状态机

```text
pending --OIDC callback--> authenticated --start--> active --end--> ended
   |                              |                   |
   +-----------过期/拒绝----------+------策略失败-----+
```

教师先在管理后台保存草稿，再执行“发布”，并将全局用户加入该考试的参加名单。
学生从裸 `grips://exam/` 进入，完成 OIDC 后直接看到被分配的考试并选择其中一场；
浏览器只提交服务端返回的 `exam_id`，不再输入或传递考试码。`starts_at` 之前允许
完成认证但不允许 `start`，`ends_at` 到达后中台会将在线 session/tunnel 置为终态。
交卷会写入 `(exam_id, subject)` 唯一完成记录，因此同一学生不能再次参加同一场考试。

只有 `active` 会话可以建立 tunnel。`browser_token` 只用于控制面获取 ticket；OIDC access token 只存在中台服务端。ticket 是随机临时 VLESS UUID，服务端从数据库映射到考试会话；数据面使用标准 VLESS v0 TCP 请求头传递 UUID 和目标地址，随后全部是源站 TLS 字节，中台不读取源站 HTTP 内容。

## 配置响应

`GET /{exam_id}/.well-known/byod-configuration` 和 `/exam/{exam_id}/.well-known/byod-configuration` 返回同一结构：

```json
{
  "version": 1,
  "exam": {
    "id": "course-101",
    "origin": "https://exam.cs.ac.cn",
    "source_url": "https://cs101.gbu.edu.cn/paper/category/exam",
    "source_origin": "https://cs101.gbu.edu.cn",
    "source_host": "cs101.gbu.edu.cn",
    "endpoint_id": "course-101",
    "transport": "vless-xhttp",
    "unlock_path": "/exam/course-101/end"
  },
  "tunnel": {
    "protocol": "vless",
    "transport": "xhttp",
    "mode": "packet-up",
    "encryption": "none",
    "endpoint": "https://exam.cs.ac.cn/v1/xhttp/",
    "endpoint_id": "course-101",
    "ticket_path": "/v1/sessions/{session_id}/tunnel-ticket"
  },
  "oidc": {
    "authorization_endpoint": "https://idp.example/authorize",
    "callback_endpoint": "/oidc/callback",
    "client_id": "byod-browser",
    "response_type": "code"
  },
  "policy": {
    "key_id": "prod-2026-01",
    "alg": "HS256",
    "document": {
      "exam_id": "course-101",
      "allowed_origins": [],
      "tunnel_hosts": ["cs101.gbu.edu.cn", "minio.cs101.gbu.edu.cn"]
    },
    "signature": "base64url(...)"
  }
}
```

策略中的 `document.browser` 是第一版考试加固基线，建议至少支持以下字段：

- `require_fullscreen`: 进入考试时将浏览器窗口切换到全屏；结束或解除会话时恢复进入考试前状态。
- `lock_fullscreen`: 必须与 `require_fullscreen` 同时为 `true` 才生效；启用后 Chromium 会拦截 Esc、F11、菜单等退出全屏操作。它不能阻止操作系统级 Alt+Tab、锁屏或任务管理器。

| 字段 | 默认值 | 含义 |
|---|---:|---|
| `allow_background` | `false` | 切换到其他应用/窗口时是否允许继续考试 |
| `allow_new_tabs` / `allow_new_windows` | `false` | 是否允许打开新标签页或新窗口 |
| `allow_devtools` | `false` | 是否允许 DevTools、远程调试入口 |
| `allow_print` / `allow_view_source` | `false` | 是否允许打印和查看源代码 |
| `allow_save_page` / `allow_downloads` | `false` | 是否允许保存页面和下载文件 |
| `allow_incognito` / `allow_extensions` | `false` | 是否允许隐身窗口和扩展 |
| `allow_clipboard_read` / `allow_clipboard_write` | `false` | 剪贴板读取/写入权限 |
| `allow_screen_capture` | `false` | 屏幕捕获、截图、录屏接口 |
| `require_fullscreen` | `false` | 进入考试时自动将浏览器窗口切换为全屏 |
| `lock_fullscreen` | `false` | 与 `require_fullscreen` 一起启用时拦截退出全屏操作 |
| `allow_navigation_outside_exam` | `false` | 是否可以离开考试 origin |
| `kiosk_mode` | `true` | 隐藏普通浏览器退出路径，进入考试壳模式 |
| `exit_requires_unlock` | `true` | 只有考试结束链接/管理员解锁才能退出 |

另外，`document.navigation.allowed_origins` 控制考试期间允许的顶层 origin。列表必须
是精确的 HTTP(S) origin（不支持通配符），例如源站使用 IAaa 登录时显式加入
`https://iaaa.gbu.edu.cn`。浏览器会将该签名列表传给 native 导航 observer；未列出的
顶层导航仍会被回退到考试页面。`document.navigation` 还控制禁止的 URL scheme，
`document.violations` 定义违规动作的处理方式（例如切后台后 `suspend`、新标签页
直接 `block`）。这些字段必须包含在签名文档中，不能由浏览器端自行修改。

透明 tunnel 的目标由同一签名文档的 `document.tunnel_hosts` 显式控制。它是精确
hostname 数组，不支持 scheme、端口、路径、通配符或凭据，例如
`["cs101.gbu.edu.cn", "minio.cs101.gbu.edu.cn"]`。只有列表中的 HTTPS 请求会被
送入 tunnel；空列表表示不走 tunnel，Base URL 不会隐式加入。Chromium native
网络层和 tunnel 服务端都会再次校验该列表。

浏览器当前会检查 `version`、`exam.id`、HTTPS `origin` 以及签名字段存在后再调用
`/start`；HMAC 的真正验签和系统级限制执行仍应在生产加固阶段接入。生产环境应
使用非对称签名（例如 Ed25519 或签名服务提供的 JWS），参考实现的 HMAC 只用于
本地联调。

## 与当前 Chromium 入口的衔接

当前入口把 `grips://exam.cs.ac.cn` 路由到 Chromium 的极小 native shell，shell 内嵌
`https://exam.cs.ac.cn` 的动态考试壳页面；业务 HTML/CSS/JS 由 byod-server 发布，
不再编译进 Chromium。Chromium 只保留 native bridge（tunnel、全屏、后台切换和导航约束）。
兼容的完成动作仍可使用 `grips://exam.cs.ac.cn/exams/{exam_id}/complete`，由 shell
转换为 action 后由 HTTPS 页面使用短期 session token 完成提交。

考试壳页面在 HTTPS origin 上执行以下流程：

1. 裸入口在当前标签页打开 Connect OIDC，回调后请求 `/v1/exams/available`。
2. 学生选择服务端返回的 `exam_id`，再从对应的考试 origin 请求
   `target + "/.well-known/byod-configuration"`。
3. 在当前标签页打开 `authorization_url`（仅未复用用户 OIDC session 时），回到 WebUI
   后轮询会话状态；未到开始时间显示倒计时并轮询 `start`。
4. active 后显示结束倒计时，到 `ends_at` 自动调用 `/v1/exams/{exam_id}/complete`。
5. 手动访问 `grips://exam.cs.ac.cn/exams/{exam_id}/complete` 由 Chromium 转换为当前标签页的 complete action；完成后清理 token、策略和 tunnel。
6. 调用 tunnel-ticket API，把 endpoint 和临时 VLESS UUID 交给浏览器原生网络层。浏览器打开 `exam.source_url`；仅 `tunnel_hosts` 中的 HTTPS 主机走 VLESS/XHTTP，不隐式包含 Base URL。UUID 最长 8 小时有效，且每次连接和已建立连接都检查会话状态、考试时间窗口及目标白名单。

特权 WebUI 的 CSP 需要把中台 origin 加入 `connect-src`。题目内容应在 HTTPS 代理 origin 下加载，不能把不可信考试 HTML 放进 `grips://` 特权 origin。

## 本地联调

```bash
go run ./byod_server/cmd/byod-server --listen 127.0.0.1:8787 \
  --exam-origin https://exam.cs.ac.cn \
  --upstream http://127.0.0.1:9000

curl http://127.0.0.1:8787/course-101/.well-known/byod-configuration
bash build/test-byod-server.sh
```

真实浏览器联调时，把 `exam.cs.ac.cn` 解析到中台的 HTTPS 入口，并使用正式证书；不要在生产环境复用开发默认密钥或明文 HTTP。

OIDC 回调完成后，中台返回 `/byod/complete?session_id=...&target=...`。Chromium
内置的 `GripsExamCallbackObserver` 只接受 `https://exam.cs.ac.cn/byod/complete`
并在当前标签页恢复 `grips://exam`，因此不会新开标签页。联调阶段可加
`--dev-auth` 使用中台内置回调适配器；该模式只用于开发，不提供真实身份认证。

## VLESS over XHTTP 数据面

外层为 HTTPS，复用网站域名/端口和 Istio HTTPS listener。HTTPRoute 使用
`/v1/xhttp` 路径前缀；没有 CONNECT、WebSocket Upgrade 或独立公网 TCP 端口。

当前固定采用与 Xray-core `d2758a02` 互通的 packet-up 子集：
GET `/v1/xhttp/{transport-session-uuid}` 持续下载，
POST `/v1/xhttp/{transport-session-uuid}/{sequence}` 上传二进制数据，序号从 0 开始。
请求体是连续字节流的分片，不增加自定义帧；允许有界乱序，拒绝冲突分片及超限缓冲。

流开头是标准 VLESS v0：版本、16 字节认证 UUID、零 addons、TCP 命令、目标端口、
地址类型和地址。响应头为两个零字节。VLESS encryption=none、flow 为空，不启用
UDP、Mux、Vision。原生客户端在收到 POST 200 后即可写入源站 TLS ClientHello，
不能等待源站返回数据后才开始 TLS 握手。VLESS 认证 UUID 与 XHTTP transport session
UUID 是两个独立随机值；后者只关联上下行，不能替代认证。

浏览器在 Network Service 内实现原生 StreamSocket，Chromium 的源站 TLS 层直接
使用该流。外层 HTTPS 请求禁用 cookie、HTTP 身份认证、重定向及缓存，显式绕过
考试 tunnel，防止递归。会话清理会撤销其所有 socket，不依赖 renderer 存活。
使用 Chromium 已有代理路由/连接池选择目标，但不在线上发送 HTTP CONNECT，
也不创建本地代理 listener。

服务端内存管理 XHTTP 流；扩容前必须配置按 transport session 的一致路由。
生产环境同一镜像分别运行 `--role=control`、`--role=data`，使用独立 Deployment
和 Service，均为单副本 Recreate。`/v1/xhttp` 转发到 data，其余路径转发到 control。
data 不暴露登录或管理 API，不加载 OIDC 客户端、管理员或策略签名密钥；仅持有
数据库配置、公开的 issuer 和目标 host 策略。凭证、会话、名单、时间窗口从共享
PostgreSQL 校验，已有连接每 2 秒重查并在错误时关闭。数据面只更新活跃会话的
心跳时间，状态流转仍由控制面负责；控制面读取持久化心跳并原子执行 idle 暂停。
CDN/Ingress 应禁止缓存和响应缓冲，允许长 GET；request/backend timeout=0 不等于
自动关闭每层设备的 idle timeout，须在发布前实测。旧 raw tunnel 入口及按来源 IP
选择 endpoint 的配置已移除，不支持旧浏览器协议。
