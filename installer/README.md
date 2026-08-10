# MoveToPlay Companion 队友安装包

运行 `build-team-installer.ps1` 会生成 win-x64 自包含发布版、注入本机外部保存的团队凭据、执行真实云端冒烟测试，再调用 Inno Setup 生成安装程序。私钥、API Token、引导凭据与最终安装包都位于 Git 忽略的本机目录或 `output/`，不会提交到仓库。

首次运行安装版时，`team-cloud.bootstrap.json` 会迁移到当前 Windows 用户的 DPAPI 加密文件并从安装目录删除。服务端对应公钥必须使用以下 `authorized_keys` 限制，只允许转发到回环地址的训练 API：

```text
restrict,port-forwarding,permitopen="127.0.0.1:8000",command="/bin/false"
```

构建命令：

```powershell
pwsh -File installer\build-team-installer.ps1
```

最终文件：

```text
output\installer\MoveToPlay.Companion.Team.Setup.exe
```

这是队友内部版本。安装包内的引导凭据可以被有意拆包的人读取；密钥已限制为 API 端口转发，但安装包仍不应公开发布。公开发行前应切换为 HTTPS 和独立用户令牌。
