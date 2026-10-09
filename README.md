# BYOD Exam Browser

基于 Fork Chromium 的 BYOD 考试浏览器（Windows + macOS）。

公开 fork：[`Bit-Precise/byod-browser`](https://github.com/Bit-Precise/byod-browser)。
`byod/tools` 保存补丁、构建/签名工具及验证文档；
[`byod/155`](https://github.com/Bit-Precise/byod-browser/tree/byod/155)
保存 Chromium **155.0.8059.40** 的源码分支，保留上游历史并包含当前 BYOD 改动。
源码获取、依赖同步及快照校验见 [Chromium 源码说明](docs/chromium-source.md)。

## 目录

- [`docs/superpowers/specs/`](docs/superpowers/specs/) — 架构设计与 spec
- [`docs/superpowers/plans/`](docs/superpowers/plans/) — 实施计划（roadmap + P0..P11）
- [`docs/development/`](docs/development/) — 开发者环境搭建、patch 流程、rebase 手册
- [`patches/`](patches/) — Chromium 补丁集（P1 交付）
- [`build/`](build/) — 构建脚本与 gn args
- [`ci/`](ci/) — CI 配置
- [`Bit-Precise/byod-server`](https://github.com/Bit-Precise/byod-server) — 独立维护的 BYOD 中台
- [中台 Helm chart](https://github.com/Bit-Precise/byod-server/tree/main/helm/byod-server) — Go 中台 Helm chart
- [`docs/byod-server-contract.md`](docs/byod-server-contract.md) — 中台与 grips://exam 接口契约

## 快速开始

见 [Chromium 源码说明](docs/chromium-source.md) 和
[Windows 构建流程](docs/chromium-build-workflow.md)。

### Connect 登录

BYOD Browser 不会把 Connect 账号伪装成 Chromium 的 GAIA/Google 账号，也不
会把 Connect cookie 复制到考试站点。打开 `grips://login` 后，浏览器会在当前
profile 的当前标签页顶层打开 `https://connect.cs.ac.cn/`；登录 cookie 因此由
Chromium 正常保存。头像菜单、设置页以及浏览器命令中的 Chromium 登录/重新
认证入口在 BYOD 构建中也会导航到同一个 `grips://login` 页面，不再启动
Google 登录流程。之后打开 `grips://exam.cs.ac.cn` 时，Chromium 只提供一个极小的
native shell，实际考试壳 UI 从 `https://exam.cs.ac.cn` 动态加载；中台会按 OIDC 身份列出该用户
关联的考试；选择考试后，`connect.cs.ac.cn` 作为 OIDC issuer，Connect 的现有会话会自动完成 SSO，
考试回调仍由 `exam.cs.ac.cn/oidc/callback` 服务端验证。

这条路径保留 Chromium 原生 Google Sign-in/DICE/Sync 协议实现，避免把 Google
专有 token、scope 和 profile identity 模型替换成不兼容的自定义协议；只有
面向用户的登录入口改为 Connect，BYOD 考试身份与浏览器 profile 的 Connect
Web 会话仍通过标准 OIDC SSO 解耦协作。

BYOD 构建不要求配置 Google API key。补丁
[`1301-disable-google-api-key-infobar.patch`](patches/1301-disable-google-api-key-infobar.patch)
移除了启动时创建缺少 Google API key 提示条的逻辑，不影响 Connect/OIDC 登录。
这不是对 Chromium 所有 Google 服务组件的删除；补丁不修改 GN 配置或依赖图，
可在现有 `out\Grips` 上增量编译。

### 生成 Windows 安装包

在 Chromium Windows 构建机上，先完成 canonical `out\\Grips` 构建，再运行：

```cmd
python3 C:\Users\mryao\Downloads\make-byod-installer.py ^
  --root C:\src\chromium\src ^
  --out C:\src\chromium\src\out\Grips ^
  --output C:\Users\mryao\Downloads\BYOD-Browser-Setup.exe ^
  --version 152.0.7947.0
```

脚本会先校验 `byod-build-manifest.json` 中的 SHA-256，避免把不同构建的
`chrome.exe`、`chrome.dll` 或 pak 文件混装进安装包。安装器是 per-user
Windows `.exe`，安装到 `%LOCALAPPDATA%\\BYOD Browser`，创建 Start Menu/Desktop
快捷方式，并默认打开 `grips://exam.cs.ac.cn/`。快捷方式直接使用安装目录中的
`chrome.exe` 和 Chromium 默认 profile，确保与用户手动双击浏览器的行为一致。当前 Windows 构建机的
IExpress 组件不可用时，脚本会自动使用 .NET Framework 自解压 fallback，
安装过程中会显示按实际解压字节数更新的进度条，并设置 Chromium Windows 沙箱
所需的应用包只读权限；不需要额外安装 NSIS 或其他第三方打包器。使用
`--no-launch` 可关闭安装完成
后的自动启动，使用 `--keep-work` 可保留临时 staging 目录排查包内容。

拿到安装包后也可以直接双击 [`test-byod-installer.bat`](test-byod-installer.bat)
做冒烟测试。它会自动寻找当前目录或 `dist\\` 下的安装器，校验同名 `.sha256`
文件（如果存在），在临时 `%LOCALAPPDATA%` 下安装，并验证核心运行时文件和
manifest；测试目录默认保留以便排查，传入 `/cleanup /nopause` 可在命令行测试后
自动清理且不暂停窗口。

安装并实际启动浏览器则双击 [`run-byod-browser.bat`](run-byod-browser.bat)。
它会把浏览器安装到当前用户的 `%LOCALAPPDATA%\\BYOD Browser`，使用独立的
`User Data` profile，然后打开 `grips://exam.cs.ac.cn/`；也可以把 URL 作为
第一个参数传入，例如 `run-byod-browser.bat grips://login/`。这个脚本会真实
启动浏览器，适合做 Connect 登录和考试壳联调。

### Azure Artifact Signing

签名是已有构建产物的后处理，不运行 GN/Ninja，也不修改 `out\Grips`。
安装 [微软签名客户端工具](https://learn.microsoft.com/azure/artifact-signing/how-to-signing-integrations)
和 Azure CLI 后，执行签名的身份需要有对应证书配置上的
`Artifact Signing Certificate Profile Signer` 权限。Windows SSH 会话应使用禁用
WAM 的设备码登录（在 CMD 中执行）：

```cmd
set AZURE_CORE_ENABLE_BROKER_ON_WINDOWS=false
az login --tenant <tenant-id> --use-device-code --scope https://codesigning.azure.net/.default
```

将打包脚本及其相对路径 `build\sign-windows-artifacts.ps1` 一起复制到 Windows。
`build\azure-signing.metadata.json` 记录本项目的 West US 2 账号和 Public Trust
证书配置，不包含密钥。打包时在原有命令后添加以下参数（工具路径按实际安装位置填写）：

```cmd
  --signing-metadata "C:\path\build\azure-signing.metadata.json" ^
  --signtool "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" ^
  --signing-dlib "%LOCALAPPDATA%\Microsoft\MicrosoftArtifactSigningClientTools\Azure.CodeSigning.Dlib.dll" ^
  --signing-publisher "Bit Precise, LLC"
```

流程为：校验构建 → 复制运行时 → 签名副本中的 EXE/DLL → 更新包内 manifest →
打包 → 签名安装器 → 生成 `.exe.sha256`。已有有效的第三方签名保持不变；遇到无效
签名会停止。每个新签名都包含 RFC3161 时间戳并验证发布者。包内 manifest 的
`build_files` 保留原始构建哈希，`files` 记录最终打包文件哈希。
Azure CLI 的禁用 WAM 设置在签名子进程中生效，不改变全局 Azure 配置。

`build/verify-signed-installer.ps1` 可验证当前 .NET 自解压安装器的签名、时间戳、
内嵌 ZIP 和 manifest，同时确认原构建及第三方签名文件未改变，不执行安装或改写
快捷方式。启动冒烟测试应在交互式 Windows 会话中运行；这台构建机通过 SSH 的
Session 0 启动有签名和无签名版本时，均可能遇到 `USER32.dll` 初始化失败。

## 中台镜像与 Helm 发布

独立的 [`Bit-Precise/byod-server`](https://github.com/Bit-Precise/byod-server) 仓库中的
GitHub Actions 会在 middleware 变更或版本 tag 时运行 Go 测试、构建并推送：

- `ghcr.io/<GitHub owner>/byod-server:<tag>`
- `oci://ghcr.io/<GitHub owner>/charts/byod-server:<chart version>`

工作流使用 GitHub Actions 内置的 `GITHUB_TOKEN`，仓库需要开启 Actions 的
`packages: write` 权限。`/root/k8s` 的 `nj-prod` Pulumi stack 已接入该 OCI chart；
部署前请按 [nj-prod 配置说明](/root/k8s/stacks/byod-server.md) 设置 OIDC、回源和策略签名密钥。

## 原生考试网络

指定 `tunnel_hosts` 的 HTTPS 请求通过 VLESS v0（`encryption=none`）over
XHTTP `packet-up` 传输到 `https://exam.cs.ac.cn/v1/xhttp/`。浏览器保留源站
TLS 校验；外层 HTTPS 复用现有 Istio listener，无 CONNECT、WSS 或本地代理端口。
服务端以随机临时 VLESS UUID 绑定考试会话并校验目标白名单。
同一服务端镜像分别以 control/data 角色运行：控制面处理登录与考试，数据面仅处理
XHTTP，共享 PostgreSQL。内部是两个 Deployment/Service，外部共用 HTTPS；数据面
当前单副本、Recreate，控制面重启不会打断数据流，数据面更新仍会中断已有连接。

详见 [接口契约](docs/byod-server-contract.md) 和
[浏览器原生补丁](patches/1302-vless-xhttp-tunnel.patch)。发布时必须配套更新
浏览器、服务端镜像/Helm 和 `/root/k8s` 路由；源码修改不代表已经部署。
构建、互通与 Windows 原生回归结果见 [验证记录](docs/vless-xhttp-validation.md)。

## 项目状态

v1 开发中，参见 [`docs/superpowers/plans/2026-07-11-byod-roadmap.md`](docs/superpowers/plans/2026-07-11-byod-roadmap.md)。
