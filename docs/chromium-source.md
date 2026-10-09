# Chromium 源码与补丁

GitHub 公开仓库 `Bit-Precise/byod-browser` 直接 fork 自 `chromium/chromium`，
包含两个 BYOD 分支：

- `byod/tools`：本项目的补丁、构建、打包、签名和验证工具。
- `byod/155`（默认）：Chromium 155.0.8059.40 的源码分支，保留上游提交历史，包含当前全部
  BYOD、Connect、XHTTP、页面审计及 Google 服务移除改动。

源码快照保留 Chromium 的许可证、`DEPS`、`.gitmodules` 和全部依赖 gitlink。
依赖仓库内容和 CIPD 工具由 `gclient` 获取；构建产物不在 Git 仓库中。
GitHub 复用上游仓库对象，只需推送 BYOD 的新增提交。完整检出源码及构建依赖
仍需要相应磁盘空间；本次同步没有复制上游约 66 GiB 的历史。

## 获取源码

先安装 Git、depot_tools 以及 Chromium Windows 构建要求的 Visual Studio 和 SDK，
并使 depot_tools 位于 PATH 中。在新的工作目录执行：

```cmd
git clone --single-branch --branch byod/155 https://github.com/Bit-Precise/byod-browser.git src
```

在 `src` 同级创建 `.gclient`：

```python
solutions = [{
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
}]
target_os = ["win"]
```

从 `.gclient` 所在目录运行 `gclient sync --nohooks`、`gclient runhooks`，
按快照的 DEPS 同步依赖。`managed: False` 保持 BYOD 源码分支。
不要对源码分支再次应用 `byod/tools` 中的补丁。

另行克隆 `byod/tools` 获取工具和文档：

```cmd
git clone --single-branch --branch byod/tools https://github.com/Bit-Precise/byod-browser.git byod-tools
```

[`build/chromium-155-args.gn`](../build/chromium-155-args.gn) 保存已验证构建的参数，
可作为新输出目录的起点。旧的迁移/打包 CMD 包含原构建机路径、提交和日期检查，
用于记录已执行的工作；换机器时应先核对这些前提。
遵循 [Windows 构建流程](chromium-build-workflow.md)，先检查 Ninja dry run，
成功构建后再生成对应 checkout 的 manifest。旧构建的 manifest 不能用于新 checkout。

## 可追溯性

- 上游版本：`155.0.8059.40`。
- 上游提交：`cfaadc5a132d78e1828635aa8405a499f3e14864`。
- 源码快照提交：`61512998548beae1e8152b86b5767a527f8988dd`。
- 源码 tree：`597ef07bd5d995ad6b8d09f22db446be701f1a11`。
- [快照记录](../build/chromium-155-source-snapshot.json) 包含 258 个改动文件的
  SHA-256、Windows 原 checkout 提交和构建 manifest 摘要。

2026-10-09 导出时，258 个文件全部匹配最终 Windows 验证记录
`repro155-after-audit2-20261008.json`。导出使用独立 Git index，原构建机的
工作树、当前分支、index 和构建 manifest 保持原样。

若需要沿上游历史 rebase，可从官方提交开始，按
[补丁顺序](../patches/chromium-155/README.md) 重放，再校验上述文件哈希。
`byod/155` 的两个 BYOD 提交直接建立在上述上游提交之上，可正常比较和 rebase。

## 验证范围

发布前本地 41 项构建、签名、服务拦截和 NetLog 工具测试通过。
此前的 Windows 编译、原生运行和签名安装包验证见
[验证记录](chromium-upstream-google-free.md)；此次源码上传没有重新编译浏览器。
该版本仍是测试构建，Google 服务网络路径的全面覆盖尚未验证。
中台源码独立维护于 [Bit-Precise/byod-server](https://github.com/Bit-Precise/byod-server)。
