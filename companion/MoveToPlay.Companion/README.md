# MoveToPlay Companion Prototype 01

这是 MoveToPlay 的 Windows 桌面端运动伴侣。当前版本默认通过 Dongle 的 USB CDC 复合接口接收真实动作数据，同时保留显式模拟模式用于没有硬件时预览界面。

主窗口的“数据采集与云端训练”入口还可以读取 Dongle 运行时采集态的原始六轴数据、维护可编辑动作事件库、预选本次动作，并用 Blade 单击完成即时或倒计时标记。Tracker 与 Blade 使用统一 Dongle 接收时间对齐，即时方式支持延迟补偿；事件库保存在“文档/MoveToPlay/event_catalog.json”。“我的采集数据”按会话列出全部本地历史记录，玩家可任意勾选合并、定向删除自己的会话；每次提交训练都必须先经过该页。所选玩家会话只叠加到服务器固定的只读官方数据集，官方数据不会被删改或重复累积。统一 Dongle 固件每次长按按钮 2 秒按“绿色 Play → 橙色采集 → 蓝色 Wi-Fi → 重启回绿色 Play”循环，不再为采集单独烧录固件。训练通过后，服务器会把模型 C 数组集成进 Dongle 工程并编译为完整固件包；玩家确认采用后，软件只下载和校验固件，再用内置烧录工具写入蓝灯维护模式的 Dongle，本机无需 ESP-IDF。队友安装包内置只能转发训练 API 的受限 SSH 身份，首次运行后使用 Windows DPAPI 加密保存凭据；开发构建在没有团队凭据时仍使用本机 `movetoplay-server` SSH 配置。云端 API 继续只监听服务器 `127.0.0.1:8000`。

- 透明置顶、鼠标点击穿透的游戏悬浮层；主窗口不再绘制模拟游戏预览，启用时直接覆盖目标游戏所在的整个显示器；
- 自动选择目标游戏所在显示器，并以整个屏幕作为悬浮层坐标范围；
- 切出游戏时自动隐藏；
- 悬浮层只绘制文字、状态点和目标进度线，不显示任何卡片底色或边框；
- 文字使用字形级黑色描边与零偏移柔和阴影，在明暗复杂的游戏画面上仍可辨认；
- 8 套配置化主题：原神、鸣潮、我的世界、艾尔登法环、GTA V、赛博朋克 2077、通用和科幻竞技；
- 每套主题可独立配置文字、标记符号、字体、进度线样式、悬浮位置和运动激励；
- 当前动作、心率、估算卡路里、运动时间和连击展示；
- 游戏化运动激励动画；
- `Ctrl + Shift + F10` 全局显示/隐藏快捷键。
- 点击主窗口右上角关闭按钮后隐藏到 Windows 系统托盘，动作接收和统计继续运行；双击托盘图标恢复，右键菜单可真正退出。
- 主界面只保留“打开悬浮层控制”入口；独立置顶控制窗打开时自动显示真实悬浮层，可边看游戏画面边调整九宫格锚点、水平/垂直偏移，并分别缩放主题标题、当前动作、运动统计、目标进度和激励提示。每个主题与分辨率的设置自动保存到 `%LOCALAPPDATA%\MoveToPlay\overlay-placements.json`。

## 运行

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj
```

或者构建后运行：

```powershell
dotnet build companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj -c Release
companion/MoveToPlay.Companion/bin/Release/net8.0-windows/MoveToPlay.Companion.exe
```

软件默认自动查找 USB VID `303A`、PID `4005` 的 MoveToPlay Dongle，并在设备插拔后自动重连。也可以手动指定端口和用户体重：

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj -- --port=COM35 --weight=68
```

只有需要预览随机模拟数据时才使用：

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj -- --demo
```

## Dongle 遥测协议

源码开发环境的云端功能要求 `ssh movetoplay-server` 已经可以通过密钥登录；队友安装包不需要提前配置 SSH。可运行客户端冒烟测试：

```powershell
dotnet run --project companion/MoveToPlay.Companion.Smoke -c Release

# 验证本地采集会话的扫描、选择合并和定向删除
dotnet run --project companion/MoveToPlay.Companion.Smoke -c Release -- --collection-library

# 验证已下载云端固件包的清单、大小与 SHA-256（不烧录）
dotnet run --project companion/MoveToPlay.Companion.Smoke -c Release -- `
  --firmware-package JOB_ID firmware-bundle.zip

# 同时验证 C# 分块上传与云端 validate Worker
dotnet run --project companion/MoveToPlay.Companion.Smoke -c Release -- `
  data/zhq12.csv data/zhq12_events.csv
```

Dongle 在 Play 模式下枚举为 HID 键盘/鼠标与 CDC 虚拟串口复合设备。CDC 每 100 ms 发送一行 UTF-8 JSON，电脑端以换行符分包。当前协议版本为 1，内容包括：

- Dongle 序号与单调时间戳；
- 当前动作、识别置信度、运动强度和有效运动状态；
- 实际键鼠动作事件与累计事件数；
- 四个 Tracker 的在线掩码、在线数量和信号质量；界面每秒检查最新数据，串口长时间无数据时自动重连；
- Blade 在线状态及各设备电量。

电脑端根据动作 MET、运动强度、有效运动时间和用户体重估算卡路里。默认体重暂为 68 kg，可用 `--weight` 修改。识别置信度只表示模型对动作类别的确定程度，不作为运动强度使用。

云端训练页会显示具体训练阶段、进度、已用时间和预计剩余时间；训练期间不再混入上一次模型指标，完成后才展示准确率与质量门禁。训练通过后由服务器集成 C 数组、编译并打包完整 Dongle 固件，电脑端只下载和校验固件，再调用随软件提供的官方烧录工具。独立的“模型版本库”页支持让云端重新编译历史固件，以及选择蓝灯维护串口后一键回滚并烧录。电脑端无需安装 ESP-IDF。

## 当前边界

- 默认数据源是 `UsbCdcTelemetryService`；`DemoTelemetryService` 只在 `--demo` 模式启用。
- MAX30102 平时保持低功耗关闭；玩家点击“结束运动并测量心率”后，Companion 经 USB CDC 与 ESP-NOW 唤醒 Blade，检测手指后静止测量 10 秒，完成后自动关闭并生成本地运动报告。测量失败时会根据有效运动时间、动作强度与估算消耗给出明确标注为“非传感器实测”的估算心率，保证报告仍可生成。
- 当前卡路里是基于动作 MET、Dongle 运动强度、有效运动时间和体重的估算值，不用于医疗判断。
- WPF 置顶层适用于窗口化和无边框全屏。真正的独占全屏可能覆盖普通桌面窗口。
- 悬浮层不注入游戏进程，只创建独立透明窗口。

## 后续接入点

数据源统一实现 `Services/ITelemetrySource.cs`。真实串口由 `UsbCdcTelemetryService` 负责自动发现、解析和重连，模拟预览由 `DemoTelemetryService` 提供。

主题位于 `Profiles/*.json`，后续为不同游戏增加 JSON 配置即可。自动绑定支持精确进程名，也支持“进程名 + 窗口标题”组合规则，避免误认通用的 Unreal/Java 进程。
